#import <RNReanimated/NativeMethods.h>
#import <RNReanimated/NativeProxy.h>
#import <RNReanimated/REAAnimationsManager.h>
#import <RNReanimated/REAIOSErrorHandler.h>
#import <RNReanimated/REAIOSScheduler.h>
#import <RNReanimated/REAJSIUtils.h>
#import <RNReanimated/REAKeyboardEventObserver.h>
#import <RNReanimated/REAMessageThread.h>
#import <RNReanimated/REAModule.h>
#import <RNReanimated/REANodesManager.h>
#import <RNReanimated/REAUIManager.h>
#import <RNReanimated/RNGestureHandlerStateManager.h>
#import <RNReanimated/ReanimatedRuntime.h>
#import <RNReanimated/ReanimatedSensorContainer.h>

#ifdef RCT_NEW_ARCH_ENABLED
#import <RNReanimated/ReanimatedUIManagerBinding.h>
#import <React-Fabric/react/renderer/core/ShadowNode.h>
#import <React-Fabric/react/renderer/uimanager/primitives.h>
#import <React/RCTBridge+Private.h>
#import <React/RCTScheduler.h>
#import <React/RCTSurfacePresenter.h>
#else
#import <folly/json.h>
#endif

#import <React/RCTFollyConvert.h>
#import <React/RCTUIManager.h>

#if TARGET_IPHONE_SIMULATOR
#import <dlfcn.h>
#endif

@interface RCTBridge (JSIRuntime)
- (void *)runtime;
@end

namespace reanimated {

using namespace facebook;
using namespace react;

static CGFloat SimAnimationDragCoefficient(void)
{
  static float (*UIAnimationDragCoefficient)(void) = NULL;
#if TARGET_IPHONE_SIMULATOR
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    UIAnimationDragCoefficient = (float (*)(void))dlsym(RTLD_DEFAULT, "UIAnimationDragCoefficient");
  });
#endif
  return UIAnimationDragCoefficient ? UIAnimationDragCoefficient() : 1.f;
}

static CFTimeInterval calculateTimestampWithSlowAnimations(CFTimeInterval currentTimestamp)
{
#if TARGET_IPHONE_SIMULATOR
  static CFTimeInterval dragCoefChangedTimestamp = CACurrentMediaTime();
  static CGFloat previousDragCoef = SimAnimationDragCoefficient();

  const CGFloat dragCoef = SimAnimationDragCoefficient();
  if (previousDragCoef != dragCoef) {
    previousDragCoef = dragCoef;
    dragCoefChangedTimestamp = CACurrentMediaTime();
  }

  const bool areSlowAnimationsEnabled = dragCoef != 1.f;
  if (areSlowAnimationsEnabled) {
    return (dragCoefChangedTimestamp + (currentTimestamp - dragCoefChangedTimestamp) / dragCoef);
  } else {
    return currentTimestamp;
  }
#else
  return currentTimestamp;
#endif
}

// COPIED FROM RCTTurboModule.mm
static id convertJSIValueToObjCObject(jsi::Runtime &runtime, const jsi::Value &value);

static NSString *convertJSIStringToNSString(jsi::Runtime &runtime, const jsi::String &value)
{
  return [NSString stringWithUTF8String:value.utf8(runtime).c_str()];
}

static NSDictionary *convertJSIObjectToNSDictionary(jsi::Runtime &runtime, const jsi::Object &value)
{
  jsi::Array propertyNames = value.getPropertyNames(runtime);
  size_t size = propertyNames.size(runtime);
  NSMutableDictionary *result = [NSMutableDictionary new];
  for (size_t i = 0; i < size; i++) {
    jsi::String name = propertyNames.getValueAtIndex(runtime, i).getString(runtime);
    NSString *k = convertJSIStringToNSString(runtime, name);
    id v = convertJSIValueToObjCObject(runtime, value.getProperty(runtime, name));
    if (v) {
      result[k] = v;
    }
  }
  return [result copy];
}

static NSArray *convertJSIArrayToNSArray(jsi::Runtime &runtime, const jsi::Array &value)
{
  size_t size = value.size(runtime);
  NSMutableArray *result = [NSMutableArray new];
  for (size_t i = 0; i < size; i++) {
    // Insert kCFNull when it's `undefined` value to preserve the indices.
    [result addObject:convertJSIValueToObjCObject(runtime, value.getValueAtIndex(runtime, i)) ?: (id)kCFNull];
  }
  return [result copy];
}

static id convertJSIValueToObjCObject(jsi::Runtime &runtime, const jsi::Value &value)
{
  if (value.isUndefined() || value.isNull()) {
    return nil;
  }
  if (value.isBool()) {
    return @(value.getBool());
  }
  if (value.isNumber()) {
    return @(value.getNumber());
  }
  if (value.isString()) {
    return convertJSIStringToNSString(runtime, value.getString(runtime));
  }
  if (value.isObject()) {
    jsi::Object o = value.getObject(runtime);
    if (o.isArray(runtime)) {
      return convertJSIArrayToNSArray(runtime, o.getArray(runtime));
    }
    return convertJSIObjectToNSDictionary(runtime, o);
  }

  throw std::runtime_error("Unsupported jsi::jsi::Value kind");
}

static NSSet *convertProps(jsi::Runtime &rt, const jsi::Value &props)
{
  NSMutableSet *propsSet = [[NSMutableSet alloc] init];
  jsi::Array propsNames = props.asObject(rt).asArray(rt);
  for (int i = 0; i < propsNames.size(rt); i++) {
    NSString *propName = @(propsNames.getValueAtIndex(rt, i).asString(rt).utf8(rt).c_str());
    [propsSet addObject:propName];
  }
  return propsSet;
}

std::shared_ptr<NativeReanimatedModule> createReanimatedModule(
    RCTBridge *bridge,
    std::shared_ptr<CallInvoker> jsInvoker)
{
  REAModule *reanimatedModule = [bridge moduleForClass:[REAModule class]];

#ifdef RCT_NEW_ARCH_ENABLED
  // nothing
#else
  RCTUIManager *uiManager = reanimatedModule.nodesManager.uiManager;
  auto updatePropsFunction =
      [reanimatedModule](jsi::Runtime &rt, int viewTag, const jsi::Value &viewName, const jsi::Object &props) -> void {
    NSString *nsViewName = [NSString stringWithCString:viewName.asString(rt).utf8(rt).c_str()
                                              encoding:[NSString defaultCStringEncoding]];

    NSDictionary *propsDict = convertJSIObjectToNSDictionary(rt, props);
    [reanimatedModule.nodesManager updateProps:propsDict
                                 ofViewWithTag:[NSNumber numberWithInt:viewTag]
                                      withName:nsViewName];
  };

  auto measureFunction = [uiManager](int viewTag) -> std::vector<std::pair<std::string, double>> {
    return measure(viewTag, uiManager);
  };

  auto scrollToFunction = [uiManager](int viewTag, double x, double y, bool animated) {
    scrollTo(viewTag, uiManager, x, y, animated);
  };
#endif

  id<RNGestureHandlerStateManager> gestureHandlerStateManager = nil;
  auto setGestureStateFunction = [gestureHandlerStateManager, bridge](int handlerTag, int newState) mutable {
    if (gestureHandlerStateManager == nil) {
      gestureHandlerStateManager = [bridge moduleForName:@"RNGestureHandlerModule"];
    }

    setGestureState(gestureHandlerStateManager, handlerTag, newState);
  };

#ifdef RCT_NEW_ARCH_ENABLED
  // nothing
#else
  auto propObtainer = [reanimatedModule](
                          jsi::Runtime &rt, const int viewTag, const jsi::String &propName) -> jsi::Value {
    NSString *propNameConverted = [NSString stringWithFormat:@"%s", propName.utf8(rt).c_str()];
    std::string resultStr = std::string([[reanimatedModule.nodesManager obtainProp:[NSNumber numberWithInt:viewTag]
                                                                          propName:propNameConverted] UTF8String]);
    jsi::Value val = jsi::String::createFromUtf8(rt, resultStr);
    return val;
  };
#endif

  auto jsQueue = std::make_shared<REAMessageThread>([NSRunLoop currentRunLoop], ^(NSError *error) {
    throw error;
  });
  auto rnRuntime = reinterpret_cast<facebook::jsi::Runtime *>(reanimatedModule.bridge.runtime);
  std::shared_ptr<jsi::Runtime> animatedRuntime = ReanimatedRuntime::make(rnRuntime, jsQueue);

  std::shared_ptr<Scheduler> scheduler = std::make_shared<REAIOSScheduler>(jsInvoker);
  std::shared_ptr<ErrorHandler> errorHandler = std::make_shared<REAIOSErrorHandler>(scheduler);
  std::shared_ptr<NativeReanimatedModule> module;

  auto nodesManager = reanimatedModule.nodesManager;

  auto requestRender = [nodesManager, &module](std::function<void(double)> onRender, jsi::Runtime &rt) {
    [nodesManager postOnAnimation:^(CADisplayLink *displayLink) {
      double frameTimestamp = calculateTimestampWithSlowAnimations(displayLink.targetTimestamp) * 1000;
      jsi::Object global = rt.global();
      jsi::String frameTimestampName = jsi::String::createFromAscii(rt, "_frameTimestamp");
      global.setProperty(rt, frameTimestampName, frameTimestamp);
      onRender(frameTimestamp);
      global.setProperty(rt, frameTimestampName, jsi::Value::undefined());
    }];
  };

#ifdef RCT_NEW_ARCH_ENABLED
  auto synchronouslyUpdateUIPropsFunction = [nodesManager](jsi::Runtime &rt, Tag tag, const jsi::Value &props) {
    NSNumber *viewTag = @(tag);
    NSDictionary *uiProps = convertJSIObjectToNSDictionary(rt, props.asObject(rt));
    [nodesManager synchronouslyUpdateViewOnUIThread:viewTag props:uiProps];
  };

  auto progressLayoutAnimation = [=](int tag, const jsi::Object &newStyle) {
    // noop
  };

  auto endLayoutAnimation = [=](int tag, bool isCancelled, bool removeView) {
    // noop
  };

#else
  // Layout Animations start
  __block std::weak_ptr<Scheduler> weakScheduler = scheduler;
  ((REAUIManager *)uiManager).flushUiOperations = ^void() {
    std::shared_ptr<Scheduler> scheduler = weakScheduler.lock();
    if (scheduler != nullptr) {
      scheduler->triggerUI();
    }
  };

  REAUIManager *reaUiManagerNoCast = [bridge moduleForClass:[REAUIManager class]];
  RCTUIManager *reaUiManager = reaUiManagerNoCast;
  REAAnimationsManager *animationsManager = [[REAAnimationsManager alloc] initWithUIManager:reaUiManager];
  [reaUiManagerNoCast setUp:animationsManager];

  __weak REAAnimationsManager *weakAnimationsManager = animationsManager;
  std::weak_ptr<jsi::Runtime> wrt = animatedRuntime;

  auto progressLayoutAnimation = [=](int tag, const jsi::Object &newStyle) {
    NSDictionary *propsDict = convertJSIObjectToNSDictionary(*wrt.lock(), newStyle);
    [weakAnimationsManager progressLayoutAnimationWithStyle:propsDict forTag:@(tag)];
  };

  auto endLayoutAnimation = [=](int tag, bool isCancelled, bool removeView) {
    [weakAnimationsManager endLayoutAnimationForTag:@(tag) cancelled:isCancelled removeView:removeView];
  };

  auto configurePropsFunction = [reanimatedModule](
                                    jsi::Runtime &rt, const jsi::Value &uiProps, const jsi::Value &nativeProps) {
    NSSet *uiPropsSet = convertProps(rt, uiProps);
    NSSet *nativePropsSet = convertProps(rt, nativeProps);
    [reanimatedModule.nodesManager configureUiProps:uiPropsSet andNativeProps:nativePropsSet];
  };

  // Layout Animations end
#endif

  auto getCurrentTime = []() { return calculateTimestampWithSlowAnimations(CACurrentMediaTime()) * 1000; };

  // sensors
  ReanimatedSensorContainer *reanimatedSensorContainer = [[ReanimatedSensorContainer alloc] init];
  auto registerSensorFunction = [=](int sensorType, int interval, std::function<void(double[])> setter) -> int {
    return [reanimatedSensorContainer registerSensor:(ReanimatedSensorType)sensorType
                                            interval:interval
                                              setter:^(double *data) {
                                                setter(data);
                                              }];
  };

  auto unregisterSensorFunction = [=](int sensorId) { [reanimatedSensorContainer unregisterSensor:sensorId]; };
  // end sensors

  // keyboard events

  static REAKeyboardEventObserver *keyboardObserver = [[REAKeyboardEventObserver alloc] init];
  auto subscribeForKeyboardEventsFunction =
      [](std::function<void(int keyboardState, int height)> keyboardEventDataUpdater, bool isStatusBarTranslucent) {
        // ignore isStatusBarTranslucent - it's Android only
        return [keyboardObserver subscribeForKeyboardEvents:^(int keyboardState, int height) {
          keyboardEventDataUpdater(keyboardState, height);
        }];
      };

  auto unsubscribeFromKeyboardEventsFunction = [](int listenerId) {
    [keyboardObserver unsubscribeFromKeyboardEvents:listenerId];
  };
  // end keyboard events

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

  module = std::make_shared<NativeReanimatedModule>(
      jsInvoker,
      scheduler,
      animatedRuntime,
      errorHandler,
#ifdef RCT_NEW_ARCH_ENABLED
  // nothing
#else
      propObtainer,
#endif
      platformDepMethodsHolder);

  scheduler->setRuntimeManager(module);

#ifdef RCT_NEW_ARCH_ENABLED
  [reanimatedModule.nodesManager registerEventHandler:^(NSString *eventNameNSString, id<RCTEvent> event) {
    // handles RCTEvents from RNGestureHandler

    std::string eventName = [eventNameNSString UTF8String];
    jsi::Runtime &rt = *module->runtime;
    jsi::Value payload = convertNSDictionaryToJSIObject(rt, [event arguments][2]);

    module->handleEvent(eventName, std::move(payload), CACurrentMediaTime() * 1000);
  }];
#else
  [reanimatedModule.nodesManager registerEventHandler:^(NSString *eventName, id<RCTEvent> event) {
    std::string eventNameString([eventName UTF8String]);

    std::string eventAsString;
    try {
      eventAsString = folly::toJson(convertIdToFollyDynamic([event arguments][2]));
    } catch (std::exception &) {
      // Events from other libraries may contain NaN or INF values which cannot be represented in JSON.
      // See https://github.com/software-mansion/react-native-reanimated/issues/1776 for details.
      return;
    }

    eventAsString = "{ NativeMap:" + eventAsString + "}";
    jsi::Object global = module->runtime->global();
    jsi::String eventTimestampName = jsi::String::createFromAscii(*module->runtime, "_eventTimestamp");
    global.setProperty(*module->runtime, eventTimestampName, CACurrentMediaTime() * 1000);
    module->onEvent(eventNameString, eventAsString);
    global.setProperty(*module->runtime, eventTimestampName, jsi::Value::undefined());
  }];
#endif

  std::weak_ptr<NativeReanimatedModule> weakModule = module; // to avoid retain cycle
#ifdef RCT_NEW_ARCH_ENABLED
  [reanimatedModule.nodesManager registerPerformOperations:^() {
    if (auto module = weakModule.lock()) {
      module->performOperations();
    }
  }];
#else
  // Layout Animation callbacks setup
  [animationsManager setAnimationStartingBlock:^(
                         NSNumber *_Nonnull tag, NSString *type, NSDictionary *_Nonnull values, NSNumber *depth) {
    jsi::Runtime &rt = *wrt.lock();
    jsi::Object yogaValues(rt);
    for (NSString *key in values.allKeys) {
      NSNumber *value = values[key];
      yogaValues.setProperty(rt, [key UTF8String], [value doubleValue]);
    }

    weakModule.lock()->layoutAnimationsManager().startLayoutAnimation(
        rt, [tag intValue], std::string([type UTF8String]), yogaValues);
  }];

  [animationsManager setHasAnimationBlock:^(NSNumber *_Nonnull tag, NSString *_Nonnull type) {
    bool hasLayoutAnimation =
        weakModule.lock()->layoutAnimationsManager().hasLayoutAnimation([tag intValue], std::string([type UTF8String]));
    return hasLayoutAnimation ? YES : NO;
  }];

  [animationsManager setAnimationRemovingBlock:^(NSNumber *_Nonnull tag) {
    weakModule.lock()->layoutAnimationsManager().clearLayoutAnimationConfig([tag intValue]);
  }];
#endif

  return module;
}

}
