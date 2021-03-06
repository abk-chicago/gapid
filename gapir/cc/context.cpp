/*
 * Copyright (C) 2017 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "context.h"
#include "gapir/cc/gles_gfx_api.h"
#include "gles_renderer.h"
#include "interpreter.h"
#include "memory_manager.h"
#include "post_buffer.h"
#include "replay_request.h"
#include "resource_provider.h"
#include "resource_in_memory_cache.h"
#include "server_connection.h"
#include "stack.h"
#include "gapir/cc/vulkan_gfx_api.h"
#include "vulkan_renderer.h"

#include "core/cc/log.h"
#include "core/cc/target.h"

#include <cstdlib>
#include <sstream>
#include <string>

namespace gapir {

std::unique_ptr<Context> Context::create(const ServerConnection& gazer,
                                         ResourceProvider* resourceProvider,
                                         MemoryManager* memoryManager) {
    std::unique_ptr<Context> context(new Context(gazer, resourceProvider, memoryManager));

    if (context->initialize()) {
        return context;
    } else {
        return nullptr;
    }
}

// TODO: Make the PostBuffer size dynamic? It currently holds 2MB of data.
Context::Context(const ServerConnection& gazer, ResourceProvider* resourceProvider,
                 MemoryManager* memoryManager) :
        mServer(gazer), mResourceProvider(resourceProvider), mMemoryManager(memoryManager),
        mBoundGlesRenderer(nullptr), mBoundVulkanRenderer(nullptr),
        mPostBuffer(new PostBuffer(POST_BUFFER_SIZE, [this](const void* address, uint32_t count) {
            return this->mServer.post(address, count);
        })) {
}

Context::~Context() {
    for (auto it = mGlesRenderers.begin(); it != mGlesRenderers.end(); it++) {
        delete it->second;
    }
    delete mBoundVulkanRenderer;
}

bool Context::initialize() {
    mReplayRequest = ReplayRequest::create(mServer, mResourceProvider, mMemoryManager);
    if (mReplayRequest == nullptr) {
        return false;
    }

    if (!mMemoryManager->setVolatileMemory(mReplayRequest->getVolatileMemorySize())) {
        GAPID_WARNING("Setting the volatile memory size failed (size: %u)",
                     mReplayRequest->getVolatileMemorySize());
        return false;
    }

    mMemoryManager->setConstantMemory(mReplayRequest->getConstantMemory());

    return true;
}


void Context::prefetch(ResourceInMemoryCache* cache) const {
    auto cacheSize = static_cast<uint32_t>(
            static_cast<uint8_t*>(mMemoryManager->getVolatileAddress()) -
            static_cast<uint8_t*>(mMemoryManager->getBaseAddress()));
    cache->resize(cacheSize);

    auto resources = mReplayRequest->getResources();
    if (resources.size() > 0) {
        GAPID_INFO("Prefetching %d resources...", resources.size());
        mResourceProvider->prefetch(resources.data(), resources.size(), mServer,
                                    mMemoryManager->getVolatileAddress(),
                                    mReplayRequest->getVolatileMemorySize());
    }
}

bool Context::interpret() {
    Interpreter::ApiRequestCallback callback =
        [this](Interpreter* interpreter, uint8_t api_index) -> bool {
            if (api_index == gapir::Vulkan::INDEX) {
                // There is only one vulkan "renderer" so we create it when requested.
                mBoundVulkanRenderer = VulkanRenderer::create();
                if (mBoundVulkanRenderer->isValid()) {
                    Api* api = mBoundVulkanRenderer->api();
                    interpreter->setRendererFunctions(api->index(), &api->mFunctions);
                    GAPID_INFO("Bound Vulkan renderer");
                    return true;
                }
            }
            return false;
        };

    mInterpreter.reset(new Interpreter(mMemoryManager, mReplayRequest->getStackSize(), std::move(callback)));
    registerCallbacks(mInterpreter.get());
    auto res = mInterpreter->run(mReplayRequest->getInstructionList()) && mPostBuffer->flush();
    mInterpreter.reset(nullptr);
    return res;
}

void Context::onDebugMessage(int severity, const char* msg) {
    auto label = mInterpreter->getLabel();
    std::string tmp;
    if (msg != nullptr) {
      auto len = strlen(msg);
      if (len >= 1 && msg[len-1] == '\n') {
        tmp = std::string(msg, len-1);
        msg = tmp.data();
      }
    }
    switch (severity) {
    case LOG_LEVEL_ERROR:
        GAPID_ERROR("Renderer (%d): %s", label, msg);
        break;
    case LOG_LEVEL_WARNING:
        GAPID_WARNING("Renderer (%d): %s", label, msg);
        break;
    default:
        GAPID_DEBUG("Renderer (%d): %s", label, msg);
        break;
    }
}

void Context::registerCallbacks(Interpreter* interpreter) {
    // Custom function for posting and fetching resources to and from the server
    interpreter->registerBuiltin(Interpreter::GLOBAL_INDEX, Interpreter::POST_FUNCTION_ID,
                                 [this](Stack* stack, bool) { return this->postData(stack); });
    interpreter->registerBuiltin(Interpreter::GLOBAL_INDEX, Interpreter::RESOURCE_FUNCTION_ID,
                                 [this](Stack* stack, bool) { return this->loadResource(stack); });

    // Registering custom synthetic functions
    interpreter->registerBuiltin(Gles::INDEX, Builtins::StartTimer,
                                 [this](Stack* stack, bool) { return this->startTimer(stack); });
    interpreter->registerBuiltin(Gles::INDEX, Builtins::StopTimer,
                                 [this](Stack* stack, bool pushReturn) {
        return this->stopTimer(stack, pushReturn);
    });
    interpreter->registerBuiltin(Gles::INDEX, Builtins::FlushPostBuffer, [this](Stack* stack, bool) {
        return this->flushPostBuffer(stack);
    });

    interpreter->registerBuiltin(Gles::INDEX, Builtins::ReplayCreateRenderer, [this](Stack* stack, bool) {
        uint32_t id = stack->pop<uint32_t>();
        if (stack->isValid()) {
            GAPID_INFO("replayCreateRenderer(%u)", id);
            if (Renderer* prev = mGlesRenderers[id]) {
                if (mBoundGlesRenderer == prev) {
                    mBoundGlesRenderer = nullptr;
                }
                delete prev;
            }
            // Share objects with the root GLES context.
            // This will essentially make all objects shared between all contexts.
            // It is ok since correct replay will only reference what it is supposed to.
            if (!mRootGlesRenderer) {
                mRootGlesRenderer.reset(GlesRenderer::create(nullptr));
            }
            auto renderer = GlesRenderer::create(mRootGlesRenderer.get());
            renderer->setListener(this);
            mGlesRenderers[id] = renderer;
            return true;
        } else {
            GAPID_WARNING("Error during calling function replayCreateRenderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Gles::INDEX, Builtins::ReplayBindRenderer, [this, interpreter](Stack* stack, bool) {
        uint32_t id = stack->pop<uint32_t>();
        if (stack->isValid()) {
            GAPID_DEBUG("replayBindRenderer(%u)", id);
            if (mBoundGlesRenderer != nullptr) {
                mBoundGlesRenderer->unbind();
                mBoundGlesRenderer = nullptr;
            }
            mBoundGlesRenderer = mGlesRenderers[id];
            mBoundGlesRenderer->bind();
            Api* api = mBoundGlesRenderer->api();
            interpreter->setRendererFunctions(api->index(), &api->mFunctions);
            GAPID_DEBUG("Bound renderer %u: %s - %s", id, mBoundGlesRenderer->name(), mBoundGlesRenderer->version());
            return true;
        } else {
            GAPID_WARNING("Error during calling function replayBindRenderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Gles::INDEX, Builtins::ReplayChangeBackbuffer, [this](Stack* stack, bool) {
        GlesRenderer::Backbuffer backbuffer;

        bool resetViewportScissor = stack->pop<bool>();
        backbuffer.format.stencil = stack->pop<uint32_t>();
        backbuffer.format.depth = stack->pop<uint32_t>();
        backbuffer.format.color = stack->pop<uint32_t>();
        backbuffer.height = stack->pop<int32_t>();
        backbuffer.width = stack->pop<int32_t>();

        if (!stack->isValid()) {
            GAPID_WARNING("Error during calling function replayCreateRenderer");
            return false;
        }

        if (stack->isValid()) {
            GAPID_INFO("contextInfo(%d, %d, 0x%x, 0x%x, 0x%x)",
                    backbuffer.width,
                    backbuffer.height,
                    backbuffer.format.color,
                    backbuffer.format.depth,
                    backbuffer.format.stencil,
                    resetViewportScissor ? "true" : "false");
            if (mBoundGlesRenderer == nullptr) {
                GAPID_INFO("contextInfo called without a bound renderer");
                return false;
            }
            mBoundGlesRenderer->setBackbuffer(backbuffer);
            auto gles = mBoundGlesRenderer->getApi<Gles>();
            // TODO: This needs to change when we support other APIs.
            GAPID_ASSERT(gles != nullptr);
            if (resetViewportScissor) {
                gles->mFunctionStubs.glViewport(0, 0, backbuffer.width, backbuffer.height);
                gles->mFunctionStubs.glScissor(0, 0, backbuffer.width, backbuffer.height);
            }
            return true;
        } else {
            GAPID_WARNING("Error during calling function replayCreateRenderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Vulkan::INDEX, Builtins::ReplayCreateVkInstance,
                                 [this, interpreter](Stack* stack, bool pushReturn) {
        GAPID_DEBUG("replayCreateVkInstance()");

        if (mBoundVulkanRenderer != nullptr || interpreter->registerApi(Vulkan::INDEX)) {
            auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
            return api->replayCreateVkInstance(stack, pushReturn);
        } else {
            GAPID_WARNING("replayCreateVkInstance called without a bound Vulkan renderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Vulkan::INDEX, Builtins::ReplayCreateVkDevice,
                                 [this, interpreter](Stack* stack, bool pushReturn) {
        GAPID_DEBUG("replayCreateVkDevice()");
        if (mBoundVulkanRenderer != nullptr) {
            auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
            return api->replayCreateVkDevice(stack, pushReturn);
        } else {
            GAPID_WARNING("replayCreateVkDevice called without a bound Vulkan renderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Vulkan::INDEX, Builtins::ReplayRegisterVkInstance,
                                 [this, interpreter](Stack* stack, bool) {
        GAPID_DEBUG("replayRegisterVkInstance()");
        if (mBoundVulkanRenderer != nullptr) {
            auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
            return api->replayRegisterVkInstance(stack);
        } else {
            GAPID_WARNING("replayRegisterVkInstance called without a bound Vulkan renderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Vulkan::INDEX, Builtins::ReplayUnregisterVkInstance,
                                 [this, interpreter](Stack* stack, bool) {
        GAPID_DEBUG("replayUnregisterVkInstance()");
        if (mBoundVulkanRenderer != nullptr) {
            auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
            return api->replayUnregisterVkInstance(stack);
        } else {
            GAPID_WARNING("replayUnregisterVkInstance called without a bound Vulkan renderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Vulkan::INDEX, Builtins::ReplayRegisterVkDevice,
                                 [this, interpreter](Stack* stack, bool) {
        GAPID_DEBUG("replayRegisterVkDevice()");
        if (mBoundVulkanRenderer != nullptr) {
            auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
            return api->replayRegisterVkDevice(stack);
        } else {
            GAPID_WARNING("replayRegisterVkDevice called without a bound Vulkan renderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Vulkan::INDEX, Builtins::ReplayUnregisterVkDevice,
                                 [this, interpreter](Stack* stack, bool) {
        GAPID_DEBUG("replayUnregisterVkDevice()");
        if (mBoundVulkanRenderer != nullptr) {
            auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
            return api->replayUnregisterVkDevice(stack);
        } else {
            GAPID_WARNING("replayUnregisterVkDevice called without a bound Vulkan renderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Vulkan::INDEX, Builtins::ReplayRegisterVkCommandBuffers,
                                 [this, interpreter](Stack* stack, bool) {
        GAPID_DEBUG("replayRegisterVkCommandBuffers()");
        if (mBoundVulkanRenderer != nullptr) {
            auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
            return api->replayRegisterVkCommandBuffers(stack);
        } else {
            GAPID_WARNING("replayRegisterVkCommandBuffers called without a bound Vulkan renderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Vulkan::INDEX, Builtins::ReplayUnregisterVkCommandBuffers,
                                 [this, interpreter](Stack* stack, bool) {
        GAPID_DEBUG("replayUnregisterVkCommandBuffers()");
        if (mBoundVulkanRenderer != nullptr) {
            auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
            return api->replayUnregisterVkCommandBuffers(stack);
        } else {
            GAPID_WARNING("replayUnregisterVkCommandBuffers called without a bound Vulkan renderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Vulkan::INDEX, Builtins::ToggleVirtualSwapchainReturnAcquiredImage,
                                 [this, interpreter](Stack* stack, bool) {
        GAPID_DEBUG("ToggleVirtualSwapchainReturnAcquiredImage()");
        if (mBoundVulkanRenderer != nullptr) {
            auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
            return api->toggleVirtualSwapchainReturnAcquiredImage(stack);
        } else {
            GAPID_WARNING("toggleVirtualSwapchainReturnAcquiredImage called without a bound Vulkan renderer");
            return false;
        }
    });

    interpreter->registerBuiltin(
        Vulkan::INDEX,
        Builtins::ReplayAllocateImageMemory,
        [this, interpreter](Stack* stack, bool push_return) {
            GAPID_DEBUG("replayAllocateImageMemory()");
            if (mBoundVulkanRenderer != nullptr) {
                auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
                return api->replayAllocateImageMemory(stack, push_return);
            } else {
                GAPID_WARNING("replayAllocateImageMemory called without a "
                              "bound Vulkan renderer");
                return false;
            }
        });

    interpreter->registerBuiltin(Vulkan::INDEX, Builtins::ReplayGetFenceStatus,
                                 [this, interpreter](Stack* stack, bool push_return) {
        GAPID_DEBUG("ReplayGetFenceStatus()");
        if (mBoundVulkanRenderer != nullptr) {
            auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
            return api->replayGetFenceStatus(stack, push_return);
        } else {
            GAPID_WARNING("ReplayGetFenceStatus called without a bound Vulkan renderer");
            return false;
        }
    });

    interpreter->registerBuiltin(Vulkan::INDEX, Builtins::ReplayGetEventStatus,
                                 [this, interpreter](Stack* stack, bool push_return) {
        GAPID_DEBUG("ReplayGetEventStatus()");
        if (mBoundVulkanRenderer != nullptr) {
            auto* api = mBoundVulkanRenderer->getApi<Vulkan>();
            return api->replayGetEventStatus(stack, push_return);
        } else {
            GAPID_WARNING("ReplayGetEventStatus called without a bound Vulkan renderer");
            return false;
        }
    });
}

bool Context::loadResource(Stack* stack) {
    uint32_t resourceId = stack->pop<uint32_t>();
    void* address = stack->pop<void*>();

    if (!stack->isValid()) {
        GAPID_WARNING("Error during loadResource");
        return false;
    }

    const auto& resource = mReplayRequest->getResources()[resourceId];

    if (!mResourceProvider->get(&resource, 1, mServer, address, resource.size)) {
        GAPID_WARNING("Can't fetch resource: %s", resource.id.c_str());
        return false;
    }

    return true;
}

bool Context::postData(Stack* stack) {
    const uint32_t count = stack->pop<uint32_t>();
    const void* address = stack->pop<const void*>();

    if (!stack->isValid()) {
        GAPID_WARNING("Error during postData");
        return false;
    }

    return mPostBuffer->push(address, count);
}

bool Context::flushPostBuffer(Stack* stack) {
    if (!stack->isValid()) {
        GAPID_WARNING("Error during flushPostBuffer");
        return false;
    }

    return mPostBuffer->flush();
}

bool Context::startTimer(Stack* stack) {
    size_t index = static_cast<size_t>(stack->pop<uint8_t>());
    if (stack->isValid()) {
        if (index < MAX_TIMERS) {
            GAPID_INFO("startTimer(%d)", index);
            mTimers[index].Start();
            return true;
        } else {
            GAPID_WARNING("StartTimer called with invalid index %d", index);
        }
    } else {
        GAPID_WARNING("Error while calling function StartTimer");
    }
    return false;
}

bool Context::stopTimer(Stack* stack, bool pushReturn) {
    size_t index = static_cast<size_t>(stack->pop<uint8_t>());
    if (stack->isValid()) {
        if (index < MAX_TIMERS) {
            GAPID_INFO("stopTimer(%d)", index);
            uint64_t ns = mTimers[index].Stop();
            if (pushReturn) {
                stack->push(ns);
            }
            return true;
        } else {
            GAPID_WARNING("StopTimer called with invalid index %d", index);
        }
    } else {
        GAPID_WARNING("Error while calling function StopTimer");
    }
    return false;
}

}  // namespace gapir
