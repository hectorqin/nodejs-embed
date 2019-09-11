//
//  NodeJS.cpp
//  NodeJSEmbed
//
//  Created by Luis Finke on 9/6/19.
//  Copyright © 2019 Luis Finke. All rights reserved.
//

#define NAPI_EXPERIMENTAL

#include "NodeJS.hpp"
#include <iostream>
#include <limits>
#include <list>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <nodejs/node_api.h>
#include <nodejs/node.h>
#include <uv.h>
#include "NAPI_Macros.hpp"
#include "NAPI_Types.hpp"

namespace embed::nodejs {
	std::thread nodejsMainThread;
	std::mutex nodejsMainThreadMutex;
	
	struct EventLoopWork {
		std::function<void(napi_env)> work;
	};
	
	struct EventLoop {
		uv_loop_t* uvLoop = nullptr;
		std::thread::id threadId;
		std::list<EventLoopWork> queuedWork;
		std::mutex queuedWorkMutex;
		napi_threadsafe_function queueFunction = nullptr;
	};
	
	std::list<EventLoop*> nodejsEventLoops;
	std::recursive_mutex nodejsEventLoopsMutex;
	
	void nodejs_main(int argc, char* argv[]);
	
	napi_value NativeModule_init(napi_env env, napi_value exports);
	napi_value NativeModule_send(napi_env env, napi_callback_info info);
	napi_value NativeModule_setReciever(napi_env env, napi_callback_info info);
	void NativeModule_main_cleanup(void*);
	
	void start(StartOptions options) {
		std::unique_lock<std::mutex> lock(nodejsMainThreadMutex);
		if(nodejsMainThread.joinable()) {
			throw std::logic_error("NodeJS already started");
		}
		if(options.args.size() >= (size_t)(std::numeric_limits<int>::max() - (2 + sizeof(int)))) {
			throw std::out_of_range("Too many arguments for nodejs");
		}
		// set environment variables
		if(options.modulePaths.size() > 0) {
			const char* prevNodePathStr = getenv("NODE_PATH");
			size_t prevNodePathLen = (prevNodePathStr != nullptr) ? strlen(prevNodePathStr) : 0;
			std::string nodePath;
			size_t strLength = (prevNodePathLen > 0) ? (prevNodePathLen + 1) : 0;
			for(auto& path : options.modulePaths) {
				strLength += path.length();
			}
			strLength += (options.modulePaths.size() - 1);
			nodePath.reserve(strLength);
			size_t index = 0;
			for(auto& path : options.modulePaths) {
				nodePath += path;
				if(index < (options.modulePaths.size()-1)) {
					nodePath += ':';
				}
				index++;
			}
			if(prevNodePathLen > 0) {
				nodePath += ':';
				nodePath.append(prevNodePathStr, prevNodePathLen);
			}
			setenv("NODE_PATH", nodePath.c_str(), 1);
		}
		
		std::vector<std::string> args = {
			"node",
			"-e",
			"\nconst native_embed = process.binding(\"__native_embed\");\nconsole.log(Object.getOwnPropertyDescriptors(native_embed));\n"
		};
		args.insert(args.end(), options.args.begin(), options.args.end());
		
		size_t argsDataSize = 0;
		for(auto& arg : args) {
			argsDataSize += (arg.length() + 1);
		}
		auto argsData = std::make_unique<char[]>(argsDataSize);
		auto argsDataPtr = argsData.get();
		auto argv = std::make_unique<char*[]>(args.size());
		size_t argIndex = 0;
		for(auto& arg : args) {
			std::strcpy(argsDataPtr, arg.data());
			argv[argIndex] = argsDataPtr;
			argsDataPtr += (arg.length() + 1);
			argIndex += 1;
		}
		int argc = (int)args.size();
		
		nodejsMainThread = std::thread([argsData=std::move(argsData), argv=std::move(argv), argc]() {
			nodejs_main(argc, argv.get());
		});
	}
	
	void nodejs_main(int argc, char* argv[]) {
		int exitCode = node::Start(argc, argv);
		printf("NodeJS exited with code %i\n", exitCode);
	}
	
	
	
	std::vector<EventLoop*> getEventLoops() {
		std::unique_lock<std::recursive_mutex> lock(nodejsEventLoopsMutex);
		std::vector<EventLoop*> loops(nodejsEventLoops.begin(), nodejsEventLoops.end());
		return loops;
	}
	
	EventLoop* getMainEventLoop() {
		std::unique_lock<std::recursive_mutex> lock(nodejsEventLoopsMutex);
		if(nodejsEventLoops.size() == 0) {
			return nullptr;
		}
		return nodejsEventLoops.front();
	}
	
	EventLoop* getEventLoop(uv_loop_t* uvLoop) {
		std::unique_lock<std::recursive_mutex> lock(nodejsEventLoopsMutex);
		auto it = std::find_if(nodejsEventLoops.begin(), nodejsEventLoops.end(), [=](EventLoop* item) {
			return (item->uvLoop == uvLoop);
		});
		if(it == nodejsEventLoops.end()) {
			return nullptr;
		}
		return *it;
	}
	
	bool checkLoopValid(EventLoop* loop) {
		std::unique_lock<std::recursive_mutex> lock(nodejsEventLoopsMutex);
		for(auto cmpLoop : nodejsEventLoops) {
			if(loop == cmpLoop) {
				return true;
			}
		}
		return false;
	}
	
	void queueMain(std::function<void(napi_env)> work) {
		std::unique_lock<std::recursive_mutex> lock(nodejsEventLoopsMutex);
		EventLoop* mainLoop = getMainEventLoop();
		if(mainLoop == nullptr) {
			throw std::runtime_error("Main loop is not running");
		}
		queue(mainLoop, work);
	}
	
	void queue(EventLoop* loop, std::function<void(napi_env)> work) {
		std::unique_lock<std::recursive_mutex> lock(nodejsEventLoopsMutex);
		if(!checkLoopValid(loop)) {
			// loop is not valid
			throw std::runtime_error("Attempting to queue work on invalid NodeJS event loop");
		}
		std::unique_lock<std::mutex> loopLock(loop->queuedWorkMutex);
		loop->queuedWork.push_back({ .work = work });
		napi_status result = napi_call_threadsafe_function(loop->queueFunction, nullptr, napi_tsfn_nonblocking);
		if(result != napi_ok && result != napi_queue_full) {
			throw std::runtime_error("Failed to queue work with status " + std::to_string((long)result));
		}
	}
	
	napi_value handleLoopQueue(napi_env env, napi_callback_info info) {
		EventLoop* eventLoop = nullptr;
		NAPI_CALL(env, napi_get_cb_info(env, info, nullptr, nullptr, nullptr, (void**)&eventLoop));
		std::list<EventLoopWork> queuedWork;
		std::unique_lock<std::mutex> lock(eventLoop->queuedWorkMutex);
		queuedWork.swap(eventLoop->queuedWork);
		lock.unlock();
		for(auto& item : queuedWork) {
			item.work(env);
		}
		return nullptr;
	}
	
	
	
	napi_value NativeModule_init(napi_env env, napi_value exports) {
		// add event loop to list
		std::unique_lock<std::recursive_mutex> lock(nodejsEventLoopsMutex);
		auto nodeLoop = new EventLoop();
		// get reference to thread id
		nodeLoop->threadId = std::this_thread::get_id();
		// get reference to uv event loop
		NAPI_CALL_ELSE(env, (delete nodeLoop), napi_get_uv_event_loop(env, &(nodeLoop->uvLoop)));
		// create thread-safe function to make calls to while the thread is alive
		napi_value flushQueueFunction;
		NAPI_CALL_ELSE(env, delete nodeLoop, napi_create_function(env, "embed::nodejs::handleLoopQueue", NAPI_AUTO_LENGTH, handleLoopQueue, (void*)nodeLoop, &flushQueueFunction));
		std::ostringstream funcName;
		funcName << "NodeJS Thread Queue Function: " << nodeLoop->threadId;
		auto funcNameStr = funcName.str();
		napi_value resourceName;
		NAPI_CALL_ELSE(env, delete nodeLoop, napi_create_string_utf8(env, funcNameStr.c_str(), funcNameStr.size(), &resourceName));
		NAPI_CALL_ELSE(env, delete nodeLoop, napi_create_threadsafe_function(env, flushQueueFunction, nullptr, resourceName, 1, 1, nullptr, nullptr, nullptr, nullptr, &nodeLoop->queueFunction));
		// add a cleanup hook to remove the loop from the list when it closes
		#define RELEASE_AND_DELETE_NODELOOP \
			napi_release_threadsafe_function(nodeLoop->queueFunction, napi_tsfn_abort); \
			delete nodeLoop;
		NAPI_CALL_ELSE(env, RELEASE_AND_DELETE_NODELOOP, napi_add_env_cleanup_hook(env, NativeModule_main_cleanup, nodeLoop->uvLoop));
		#undef DELETE_NODELOOP_AND_RELEASE
		nodejsEventLoops.push_back(nodeLoop);
		lock.unlock();
		
		// define module properties
		napi_property_descriptor properties[] = {
			NAPI_METHOD_DESCRIPTOR("send", NativeModule_send),
			NAPI_METHOD_DESCRIPTOR("setReciever", NativeModule_setReciever)
		};
		NAPI_CALL(env, napi_define_properties(env, exports, sizeof(properties) / sizeof(*properties), properties));
		return exports;
	}
	
	napi_value NativeModule_send(napi_env env, napi_callback_info info) {
		size_t argc = 0;
		napi_value args[argc];
		
		NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));
		NAPI_ASSERT(env, argc == 2, "Wrong number of arguments.");
		
		std::string eventName;
		NAPI_GET_STRING_FROM_VALUE(env, eventName, args[0]);
		
		// TODO forward message to listener
		if(eventName == "response") {
			// TODO match to corresponding request
		}
		else if(eventName == "request") {
			// TODO look for function to correspond to request
		}
		
		return nullptr;
	}
	
	napi_value NativeModule_setReciever(napi_env env, napi_callback_info info) {
		
		// TODO register functions
		
		return nullptr;
	}
	
	void NativeModule_main_cleanup(void* data) {
		uv_loop_t* uvLoop = (uv_loop_t*)data;
		// remove event loop from list
		std::unique_lock<std::recursive_mutex> lock(nodejsEventLoopsMutex);
		auto it = std::find_if(nodejsEventLoops.begin(), nodejsEventLoops.end(), [=](EventLoop* item) {
			return (item->uvLoop == uvLoop);
		});
		if(it != nodejsEventLoops.end()) {
			EventLoop* loop = *it;
			std::unique_lock<std::mutex> workLock(loop->queuedWorkMutex);
			workLock.unlock();
			if(loop->queueFunction != nullptr) {
				napi_release_threadsafe_function(loop->queueFunction, napi_tsfn_abort);
			}
			nodejsEventLoops.erase(it);
			delete loop;
		}
		lock.unlock();
	}
}

NAPI_MODULE_X(__native_embed, embed::nodejs::NativeModule_init, nullptr, 0x1)
