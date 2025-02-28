#include <android/log.h>
#include <fbjni/fbjni.h>
#include <jsi/JSIDynamic.h>
#include <jsi/jsi.h>
#include <react/jni/JMessageQueueThread.h>
#include <react/jni/ReadableNativeArray.h>
#include <react/jni/ReadableNativeMap.h>

#include <memory>
#include <string>

#include "AndroidErrorHandler.h"
#include "AndroidScheduler.h"
#include "LayoutAnimationsManager.h"
#include "NativeProxy.h"
#include "PlatformDepMethodsHolder.h"
#include "ReanimatedRuntime.h"
#include "ReanimatedVersion.h"

#ifdef RCT_NEW_ARCH_ENABLED
#include "FabricUtils.h"
#include "NewestShadowNodesRegistry.h"
#include "ReanimatedUIManagerBinding.h"
#endif

namespace reanimated {

using namespace facebook;
using namespace react;

NativeProxy::NativeProxy(
    jni::alias_ref<NativeProxy::javaobject> jThis,
    jsi::Runtime *rt,
    std::shared_ptr<facebook::react::CallInvoker> jsCallInvoker,
    std::shared_ptr<Scheduler> scheduler,
    jni::global_ref<LayoutAnimations::javaobject> _layoutAnimations
#ifdef RCT_NEW_ARCH_ENABLED
    ,
    jni::alias_ref<facebook::react::JFabricUIManager::javaobject>
        fabricUIManager
#endif
    )
    : javaPart_(jni::make_global(jThis)),
      runtime_(rt),
      jsCallInvoker_(jsCallInvoker),
      layoutAnimations(std::move(_layoutAnimations)),
      scheduler_(scheduler)
#ifdef RCT_NEW_ARCH_ENABLED
      ,
      newestShadowNodesRegistry_(std::make_shared<NewestShadowNodesRegistry>())
#endif
{
#ifdef RCT_NEW_ARCH_ENABLED
  Binding *binding = fabricUIManager->getBinding();
  RuntimeExecutor runtimeExecutor = getRuntimeExecutorFromBinding(binding);
  std::shared_ptr<UIManager> uiManager =
      binding->getScheduler()->getUIManager();
  ReanimatedUIManagerBinding::createAndInstallIfNeeded(
      *rt, runtimeExecutor, uiManager, newestShadowNodesRegistry_);
#endif
}

NativeProxy::~NativeProxy() {
  // removed temporary, new event listener mechanism need fix on the RN side
  // reactScheduler_->removeEventListener(eventListener_);
}

jni::local_ref<NativeProxy::jhybriddata> NativeProxy::initHybrid(
    jni::alias_ref<jhybridobject> jThis,
    jlong jsContext,
    jni::alias_ref<facebook::react::CallInvokerHolder::javaobject>
        jsCallInvokerHolder,
    jni::alias_ref<AndroidScheduler::javaobject> androidScheduler,
    jni::alias_ref<LayoutAnimations::javaobject> layoutAnimations
#ifdef RCT_NEW_ARCH_ENABLED
    ,
    jni::alias_ref<facebook::react::JFabricUIManager::javaobject>
        fabricUIManager
#endif
) {
  auto jsCallInvoker = jsCallInvokerHolder->cthis()->getCallInvoker();
  auto scheduler = androidScheduler->cthis()->getScheduler();
  scheduler->setJSCallInvoker(jsCallInvoker);
  return makeCxxInstance(
      jThis,
      (jsi::Runtime *)jsContext,
      jsCallInvoker,
      scheduler,
      make_global(layoutAnimations)
#ifdef RCT_NEW_ARCH_ENABLED
          ,
      fabricUIManager
#endif
      /**/);
}

void NativeProxy::installJSIBindings(
    jni::alias_ref<JavaMessageQueueThread::javaobject> messageQueueThread
#ifdef RCT_NEW_ARCH_ENABLED
    ,
    jni::alias_ref<facebook::react::JFabricUIManager::javaobject>
        fabricUIManager
#endif
    /**/) {
#ifdef RCT_NEW_ARCH_ENABLED
  // nothing
#else
  auto updatePropsFunction = [this](
                                 jsi::Runtime &rt,
                                 int viewTag,
                                 const jsi::Value &viewName,
                                 const jsi::Object &props) {
    // viewName is for iOS only, we skip it here
    this->updateProps(rt, viewTag, props);
  };

  auto measureFunction =
      [this](int viewTag) -> std::vector<std::pair<std::string, double>> {
    return measure(viewTag);
  };

  auto scrollToFunction =
      [this](int viewTag, double x, double y, bool animated) -> void {
    scrollTo(viewTag, x, y, animated);
  };
#endif

  auto getCurrentTime = [this]() {
    static const auto method =
        javaPart_->getClass()->getMethod<jlong()>("getCurrentTime");
    jlong output = method(javaPart_.get());
    return static_cast<double>(output);
  };

  auto requestRender = [this, getCurrentTime](
                           std::function<void(double)> onRender,
                           jsi::Runtime &rt) {
    // doNoUse -> NodesManager passes here a timestamp from choreographer which
    // is useless for us as we use diffrent timer to better handle events. The
    // lambda is translated to NodeManager.OnAnimationFrame and treated just
    // like reanimated 1 frame callbacks which make use of the timestamp.
    auto wrappedOnRender = [getCurrentTime, &rt, onRender](double doNotUse) {
      jsi::Object global = rt.global();
      jsi::String frameTimestampName =
          jsi::String::createFromAscii(rt, "_frameTimestamp");
      double frameTimestamp = getCurrentTime();
      global.setProperty(rt, frameTimestampName, frameTimestamp);
      onRender(frameTimestamp);
      global.setProperty(rt, frameTimestampName, jsi::Value::undefined());
    };
    this->requestRender(std::move(wrappedOnRender));
  };

#ifdef RCT_NEW_ARCH_ENABLED
  auto synchronouslyUpdateUIPropsFunction =
      [this](jsi::Runtime &rt, Tag tag, const jsi::Value &props) {
        this->synchronouslyUpdateUIProps(rt, tag, props);
      };
#else
  auto propObtainer = [this](
                          jsi::Runtime &rt,
                          const int viewTag,
                          const jsi::String &propName) -> jsi::Value {
    auto method =
        javaPart_->getClass()
            ->getMethod<jni::local_ref<JString>(int, jni::local_ref<JString>)>(
                "obtainProp");
    local_ref<JString> propNameJStr =
        jni::make_jstring(propName.utf8(rt).c_str());
    auto result = method(javaPart_.get(), viewTag, propNameJStr);
    std::string str = result->toStdString();
    return jsi::Value(rt, jsi::String::createFromAscii(rt, str.c_str()));
  };

  auto configurePropsFunction = [=](jsi::Runtime &rt,
                                    const jsi::Value &uiProps,
                                    const jsi::Value &nativeProps) {
    this->configureProps(rt, uiProps, nativeProps);
  };
#endif

  auto registerSensorFunction =
      [this](int sensorType, int interval, std::function<void(double[])> setter)
      -> int {
    return this->registerSensor(sensorType, interval, std::move(setter));
  };
  auto unregisterSensorFunction = [this](int sensorId) {
    unregisterSensor(sensorId);
  };

  auto setGestureStateFunction = [this](int handlerTag, int newState) -> void {
    setGestureState(handlerTag, newState);
  };

  auto subscribeForKeyboardEventsFunction =
      [this](
          std::function<void(int, int)> keyboardEventDataUpdater,
          bool isStatusBarTranslucent) -> int {
    return subscribeForKeyboardEvents(
        std::move(keyboardEventDataUpdater), isStatusBarTranslucent);
  };

  auto unsubscribeFromKeyboardEventsFunction = [this](int listenerId) -> void {
    unsubscribeFromKeyboardEvents(listenerId);
  };

  auto jsQueue = std::make_shared<JMessageQueueThread>(messageQueueThread);
  std::shared_ptr<jsi::Runtime> animatedRuntime =
      ReanimatedRuntime::make(runtime_, jsQueue);

  auto workletRuntimeValue =
      runtime_->global()
          .getProperty(*runtime_, "ArrayBuffer")
          .asObject(*runtime_)
          .asFunction(*runtime_)
          .callAsConstructor(*runtime_, {static_cast<double>(sizeof(void *))});
  uintptr_t *workletRuntimeData = reinterpret_cast<uintptr_t *>(
      workletRuntimeValue.getObject(*runtime_).getArrayBuffer(*runtime_).data(
          *runtime_));
  workletRuntimeData[0] = reinterpret_cast<uintptr_t>(animatedRuntime.get());

  runtime_->global().setProperty(
      *runtime_, "_WORKLET_RUNTIME", workletRuntimeValue);

#ifdef RCT_NEW_ARCH_ENABLED
  runtime_->global().setProperty(*runtime_, "_IS_FABRIC", true);
#else
  runtime_->global().setProperty(*runtime_, "_IS_FABRIC", false);
#endif

  auto version = getReanimatedVersionString(*runtime_);
  runtime_->global().setProperty(*runtime_, "_REANIMATED_VERSION_CPP", version);

  std::shared_ptr<ErrorHandler> errorHandler =
      std::make_shared<AndroidErrorHandler>(scheduler_);
  std::weak_ptr<jsi::Runtime> wrt = animatedRuntime;

  auto progressLayoutAnimation = [this, wrt](
                                     int tag, const jsi::Object &newProps) {
    auto newPropsJNI = JNIHelper::ConvertToPropsMap(*wrt.lock(), newProps);
    this->layoutAnimations->cthis()->progressLayoutAnimation(tag, newPropsJNI);
  };

  auto endLayoutAnimation = [this](int tag, bool isCancelled, bool removeView) {
    this->layoutAnimations->cthis()->endLayoutAnimation(
        tag, isCancelled, removeView);
  };

  PlatformDepMethodsHolder platformDepMethodsHolder = {
      requestRender,
#ifdef RCT_NEW_ARCH_ENABLED
      synchronouslyUpdateUIPropsFunction,
#else
      updatePropsFunction,
      scrollToFunction,
      measureFunction,
      configurePropsFunction,
#endif
      getCurrentTime,
      progressLayoutAnimation,
      endLayoutAnimation,
      registerSensorFunction,
      unregisterSensorFunction,
      setGestureStateFunction,
      subscribeForKeyboardEventsFunction,
      unsubscribeFromKeyboardEventsFunction,
  };

  auto module = std::make_shared<NativeReanimatedModule>(
      jsCallInvoker_,
      scheduler_,
      animatedRuntime,
      errorHandler,
#ifdef RCT_NEW_ARCH_ENABLED
  // nothing
#else
      propObtainer,
#endif
      platformDepMethodsHolder);

  scheduler_->setRuntimeManager(module);

  _nativeReanimatedModule = module;
  std::weak_ptr<NativeReanimatedModule> weakModule = module;

#ifdef RCT_NEW_ARCH_ENABLED
  this->registerEventHandler([weakModule, getCurrentTime](
                                 std::string eventName,
                                 std::string eventAsString) {
    if (auto module = weakModule.lock()) {
      // handles RCTEvents from RNGestureHandler

      std::string eventJSON = eventAsString.substr(
          13, eventAsString.length() - 15); // removes "{ NativeMap: " and " }"
      jsi::Runtime &rt = *module->runtime;
      jsi::Value payload =
          jsi::valueFromDynamic(rt, folly::parseJson(eventJSON));
      // TODO: support NaN and INF values
      // TODO: convert event directly to jsi::Value without JSON serialization

      module->handleEvent(eventName, std::move(payload), getCurrentTime());
    }
  });
#else
  this->registerEventHandler(
      [weakModule, getCurrentTime](
          std::string eventName, std::string eventAsString) {
        if (auto module = weakModule.lock()) {
          jsi::Object global = module->runtime->global();
          jsi::String eventTimestampName =
              jsi::String::createFromAscii(*module->runtime, "_eventTimestamp");
          global.setProperty(
              *module->runtime, eventTimestampName, getCurrentTime());
          module->onEvent(eventName, eventAsString);
          global.setProperty(
              *module->runtime, eventTimestampName, jsi::Value::undefined());
        }
      });
#endif

#ifdef RCT_NEW_ARCH_ENABLED
  Binding *binding = fabricUIManager->getBinding();
  std::shared_ptr<UIManager> uiManager =
      binding->getScheduler()->getUIManager();
  module->setUIManager(uiManager);
  module->setNewestShadowNodesRegistry(newestShadowNodesRegistry_);
  newestShadowNodesRegistry_ = nullptr;
#endif
  //  removed temporary, new event listener mechanism need fix on the RN side
  //  eventListener_ = std::make_shared<EventListener>(
  //      [module, getCurrentTime](const RawEvent &rawEvent) {
  //        return module->handleRawEvent(rawEvent, getCurrentTime());
  //      });
  //  reactScheduler_ = binding->getScheduler();
  //  reactScheduler_->addEventListener(eventListener_);

  std::weak_ptr<ErrorHandler> weakErrorHandler = errorHandler;

  layoutAnimations->cthis()->setAnimationStartingBlock(
      [wrt, weakModule, weakErrorHandler](
          int tag,
          alias_ref<JString> type,
          alias_ref<JMap<jstring, jstring>> values) {
        auto &rt = *wrt.lock();
        jsi::Object yogaValues(rt);
        for (const auto &entry : *values) {
          try {
            auto key =
                jsi::String::createFromAscii(rt, entry.first->toStdString());
            auto value = stod(entry.second->toStdString());
            yogaValues.setProperty(rt, key, value);
          } catch (std::invalid_argument e) {
            if (auto errorHandler = weakErrorHandler.lock()) {
              errorHandler->setError("Failed to convert value to number");
              errorHandler->raise();
            }
          }
        }

        weakModule.lock()->layoutAnimationsManager().startLayoutAnimation(
            rt, tag, type->toStdString(), yogaValues);
      });

  layoutAnimations->cthis()->setHasAnimationBlock(
      [weakModule](int tag, const std::string &type) {
        return weakModule.lock()->layoutAnimationsManager().hasLayoutAnimation(
            tag, type);
      });

  layoutAnimations->cthis()->setClearAnimationConfigBlock(
      [weakModule](int tag) {
        weakModule.lock()->layoutAnimationsManager().clearLayoutAnimationConfig(
            tag);
      });

  runtime_->global().setProperty(
      *runtime_,
      jsi::PropNameID::forAscii(*runtime_, "__reanimatedModuleProxy"),
      jsi::Object::createFromHostObject(*runtime_, module));
}

bool NativeProxy::isAnyHandlerWaitingForEvent(std::string s) {
  return _nativeReanimatedModule->isAnyHandlerWaitingForEvent(s);
}

void NativeProxy::performOperations() {
#ifdef RCT_NEW_ARCH_ENABLED
  _nativeReanimatedModule->performOperations();
#endif
}

void NativeProxy::registerNatives() {
  registerHybrid(
      {makeNativeMethod("initHybrid", NativeProxy::initHybrid),
       makeNativeMethod("installJSIBindings", NativeProxy::installJSIBindings),
       makeNativeMethod(
           "isAnyHandlerWaitingForEvent",
           NativeProxy::isAnyHandlerWaitingForEvent),
       makeNativeMethod("performOperations", NativeProxy::performOperations)});
}

void NativeProxy::requestRender(std::function<void(double)> onRender) {
  static auto method =
      javaPart_->getClass()
          ->getMethod<void(AnimationFrameCallback::javaobject)>(
              "requestRender");
  method(
      javaPart_.get(),
      AnimationFrameCallback::newObjectCxxArgs(std::move(onRender)).get());
}

void NativeProxy::registerEventHandler(
    std::function<void(std::string, std::string)> handler) {
  static auto method =
      javaPart_->getClass()->getMethod<void(EventHandler::javaobject)>(
          "registerEventHandler");
  method(
      javaPart_.get(),
      EventHandler::newObjectCxxArgs(std::move(handler)).get());
}

#ifdef RCT_NEW_ARCH_ENABLED
// nothing
#else
void NativeProxy::updateProps(
    jsi::Runtime &rt,
    int viewTag,
    const jsi::Object &props) {
  auto method = javaPart_->getClass()
                    ->getMethod<void(int, JMap<JString, JObject>::javaobject)>(
                        "updateProps");
  method(
      javaPart_.get(), viewTag, JNIHelper::ConvertToPropsMap(rt, props).get());
}

void NativeProxy::scrollTo(int viewTag, double x, double y, bool animated) {
  auto method =
      javaPart_->getClass()->getMethod<void(int, double, double, bool)>(
          "scrollTo");
  method(javaPart_.get(), viewTag, x, y, animated);
}

std::vector<std::pair<std::string, double>> NativeProxy::measure(int viewTag) {
  auto method =
      javaPart_->getClass()->getMethod<local_ref<JArrayFloat>(int)>("measure");
  local_ref<JArrayFloat> output = method(javaPart_.get(), viewTag);
  size_t size = output->size();
  auto elements = output->getRegion(0, size);
  std::vector<std::pair<std::string, double>> result;

  result.push_back({"x", elements[0]});
  result.push_back({"y", elements[1]});

  result.push_back({"pageX", elements[2]});
  result.push_back({"pageY", elements[3]});

  result.push_back({"width", elements[4]});
  result.push_back({"height", elements[5]});

  return result;
}
#endif // RCT_NEW_ARCH_ENABLED

#ifdef RCT_NEW_ARCH_ENABLED
inline jni::local_ref<ReadableMap::javaobject> castReadableMap(
    jni::local_ref<ReadableNativeMap::javaobject> const &nativeMap) {
  return make_local(reinterpret_cast<ReadableMap::javaobject>(nativeMap.get()));
}

void NativeProxy::synchronouslyUpdateUIProps(
    jsi::Runtime &rt,
    Tag tag,
    const jsi::Value &props) {
  static const auto method =
      javaPart_->getClass()
          ->getMethod<void(int, jni::local_ref<ReadableMap::javaobject>)>(
              "synchronouslyUpdateUIProps");
  jni::local_ref<ReadableMap::javaobject> uiProps = castReadableMap(
      ReadableNativeMap::newObjectCxxArgs(jsi::dynamicFromValue(rt, props)));
  method(javaPart_.get(), tag, uiProps);
}
#endif

int NativeProxy::registerSensor(
    int sensorType,
    int interval,
    std::function<void(double[])> setter) {
  static auto method =
      javaPart_->getClass()->getMethod<int(int, int, SensorSetter::javaobject)>(
          "registerSensor");
  return method(
      javaPart_.get(),
      sensorType,
      interval,
      SensorSetter::newObjectCxxArgs(std::move(setter)).get());
}
void NativeProxy::unregisterSensor(int sensorId) {
  auto method = javaPart_->getClass()->getMethod<void(int)>("unregisterSensor");
  method(javaPart_.get(), sensorId);
}

void NativeProxy::setGestureState(int handlerTag, int newState) {
  auto method =
      javaPart_->getClass()->getMethod<void(int, int)>("setGestureState");
  method(javaPart_.get(), handlerTag, newState);
}

void NativeProxy::configureProps(
    jsi::Runtime &rt,
    const jsi::Value &uiProps,
    const jsi::Value &nativeProps) {
  auto method = javaPart_->getClass()
                    ->getMethod<void(
                        ReadableNativeArray::javaobject,
                        ReadableNativeArray::javaobject)>("configureProps");
  method(
      javaPart_.get(),
      ReadableNativeArray::newObjectCxxArgs(jsi::dynamicFromValue(rt, uiProps))
          .get(),
      ReadableNativeArray::newObjectCxxArgs(
          jsi::dynamicFromValue(rt, nativeProps))
          .get());
}

int NativeProxy::subscribeForKeyboardEvents(
    std::function<void(int, int)> keyboardEventDataUpdater,
    bool isStatusBarTranslucent) {
  auto method =
      javaPart_->getClass()
          ->getMethod<int(KeyboardEventDataUpdater::javaobject, bool)>(
              "subscribeForKeyboardEvents");
  return method(
      javaPart_.get(),
      KeyboardEventDataUpdater::newObjectCxxArgs(
          std::move(keyboardEventDataUpdater))
          .get(),
      isStatusBarTranslucent);
}

void NativeProxy::unsubscribeFromKeyboardEvents(int listenerId) {
  auto method = javaPart_->getClass()->getMethod<void(int)>(
      "unsubscribeFromKeyboardEvents");
  method(javaPart_.get(), listenerId);
}

} // namespace reanimated
