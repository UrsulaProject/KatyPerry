%config(generator=internal)
#include <objc/runtime.h>
#include <Foundation/Foundation.h>
#include <UIKit/UIKit.h>
#include <QuartzCore/QuartzCore.h>
#include <memory>
#include <os/log.h>
#include "utils.h"
#include "Jubeat/Analyzer.h"
#include "Jubeat/JBTJudgeView.h"
#include "Jubeat/JBTInstantJudgeView.h"
#define MULIST_KEY @"SHARED_KEY"
#define FPS_KEY @"KISS_FORCED_FPS"
#define INPUT_FIX_KEY @"KISS_INPUT_EDGE_FIX"
#define ANALYZER_KEY @"KISS_ENABLED_ANALYZER"

// DirectoryRedirect.m
#pragma region "Jubeat"
static id activeGameController = nil;
static unsigned int pendingButtonDown = 0;
static NSInteger selectedFPS(){
    NSInteger fps = [[NSUserDefaults standardUserDefaults] integerForKey:FPS_KEY];
    return fps == 60 || (fps == 120 && [UIScreen mainScreen].maximumFramesPerSecond >= 120) ? fps : 30;
}

static BOOL inputEdgeFixEnabled(){
    id value = [[NSUserDefaults standardUserDefaults] objectForKey:INPUT_FIX_KEY];
    return value ? [value boolValue] : YES;
}
static BOOL analyzerEnabled(){
    id value = [[NSUserDefaults standardUserDefaults] objectForKey:ANALYZER_KEY];
    return value ? [value boolValue] : NO;
}

static unsigned int buttonBitsForTouches(NSSet *touches, UIView *view){
    id controller = activeGameController;
    id renderer = [controller valueForKey:@"mainGameRenderer"];
    if (!controller || view != [controller valueForKey:@"glView"] ||
        [[renderer valueForKey:@"state"] integerValue] != 3)
        return 0;

    BOOL isPad = [[controller valueForKey:@"isPad"] boolValue];
    CGFloat scale = [[controller valueForKey:@"displayScale"] doubleValue];
    if (scale <= 0)
        return 0;
    CGFloat expansion = [[controller valueForKey:@"buttonTouchWidth"] doubleValue];
    CGFloat margin = 0;
    id delegate = [objc_getClass("JubeatAppDelegate")
        performSelector:@selector(appDelegate)];
    if (!isPad && [[delegate valueForKey:@"is4inchAspect"] boolValue])
        margin = [[renderer valueForKey:@"buttonMarginForScreen40"] doubleValue];

    unsigned int bits = 0;
    for (UITouch *touch in touches) {
        CGPoint point = [touch locationInView:view];
        point.x /= scale;
        point.y /= scale;
        for (NSInteger i = 0; i < 16; ++i) {
            NSInteger column = i % 4, row = i / 4;
            CGRect rect = isPad
                ? CGRectMake(8 + 192 * column, 264 + 192 * row, 176, 176)
                : CGRectMake(80 * column - expansion,
                             160 + margin + 80 * row - expansion,
                             80 + 2 * expansion, 80 + 2 * expansion);
            if (point.x >= rect.origin.x && point.x <= rect.origin.x + rect.size.width &&
                point.y >= rect.origin.y && point.y <= rect.origin.y + rect.size.height)
                bits |= 1u << i;
        }
    }
    return bits;
}

%group Jubeat
%hook JubeatAppDelegate
+(NSString*)appLibraryDirectory{
    return getDocumentsPath();
}
+(NSString*)appCachesDirectory{
    return getDocumentsPath();
}
-(NSString*)musicListKey{
    return MULIST_KEY;
}
%end
%hook MarkerManager
+(NSString*)getMarkerDirectoryPath{
    NSString* appendPath = [getDocumentsPath() stringByAppendingPathComponent:@"marker"];
    NSFileManager* fm = [NSFileManager defaultManager];
    if(![fm fileExistsAtPath:appendPath]){
        [fm createDirectoryAtPath:appendPath withIntermediateDirectories:YES attributes:nil error:nil];
    }
    return appendPath;
}
%end
%hook TweetResourceManager
+(NSString*)getAppendResourcePath{
    NSString* appendPath = [getDocumentsPath() stringByAppendingPathComponent:@"appendData"];
    NSFileManager* fm = [NSFileManager defaultManager];
    if(![fm fileExistsAtPath:appendPath]){
        [fm createDirectoryAtPath:appendPath withIntermediateDirectories:YES attributes:nil error:nil];
    }
    return appendPath;
}
%end

%hook MusicSelectViewController
- (void)viewDidLoad {
    %orig;
    UITapGestureRecognizer *gesture = [[UITapGestureRecognizer alloc]
        initWithTarget:self action:@selector(openBemaniSettings:)];
    gesture.numberOfTouchesRequired = 2;
    gesture.numberOfTapsRequired = 2;
    [[self view] addGestureRecognizer:gesture];
    [gesture release];
}
%new
- (void)openBemaniSettings:(UITapGestureRecognizer *)gesture {
    UIViewController *settings = [[objc_getClass("BemaniSettingsViewController") alloc]
        initWithStyle:UITableViewStyleGrouped];
    UINavigationController *navigation = [[UINavigationController alloc]
        initWithRootViewController:settings];
    navigation.modalPresentationStyle = UIModalPresentationFormSheet;
    [self presentViewController:navigation animated:YES completion:nil];
    [navigation release];
    [settings release];
}
%end

%hook GameViewController
- (void)startAnimation {
    activeGameController = self;
    pendingButtonDown = 0;
    CADisplayLink *old = [self valueForKey:@"displayLink"];
    [old invalidate];
    CADisplayLink *link = [CADisplayLink displayLinkWithTarget:self selector:@selector(loop:)];
    NSInteger fps = selectedFPS();
    if (@available(iOS 15.0, *))
        link.preferredFrameRateRange = CAFrameRateRangeMake(fps, fps, fps);
    else
        link.preferredFramesPerSecond = fps;
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
    [self setValue:link forKey:@"displayLink"];
}
- (void)dealloc {
    if (activeGameController == self)
        activeGameController = nil;
    %orig;
}
%end

%hook EAGLView
- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event {
    %orig;
    if (inputEdgeFixEnabled())
        pendingButtonDown |= buttonBitsForTouches(touches, (UIView *)self);
}
%end

%hook Sequence
- (void)judge:(unsigned int)buttonDown btnPress:(unsigned int)buttonPress {
    if (inputEdgeFixEnabled())
        buttonDown |= pendingButtonDown & buttonPress;
    pendingButtonDown = 0;
    %orig(buttonDown, buttonPress);
}
%end
%subclass BemaniSettingsViewController : UITableViewController
- (void)viewDidLoad {
    %orig;
    [(UITableViewController *)self setTitle:@"KISS"];
    [(UITableViewController *)self navigationItem].leftBarButtonItem = [[[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemDone
        target:self
        action:@selector(closeSettings)] autorelease];

    UIView *background = [[[UIView alloc]
        initWithFrame:CGRectMake(0, 0, 0, 0)] autorelease];
    UILabel *version = [[[UILabel alloc]
        initWithFrame:CGRectMake(0, 0, 0, 0)] autorelease];
    UIFontDescriptorSymbolicTraits traits =
        UIFontDescriptorTraitBold | UIFontDescriptorTraitItalic;
    UIFontDescriptor *descriptor = [[[UIFont systemFontOfSize:
        [UIFont smallSystemFontSize]] fontDescriptor]
        fontDescriptorWithSymbolicTraits:traits];
    version.font = [UIFont fontWithDescriptor:descriptor size:0];
    version.text = [NSString stringWithFormat:@"KISS %s @ %s",GIT_COMMIT_HASH,GIT_REFSPEC];
    version.textAlignment = NSTextAlignmentCenter;
    version.textColor = [UIColor grayColor];
    version.translatesAutoresizingMaskIntoConstraints = NO;
    [background addSubview:version];
    [NSLayoutConstraint activateConstraints:@[
        [version.leadingAnchor constraintEqualToAnchor:background.leadingAnchor],
        [version.trailingAnchor constraintEqualToAnchor:background.trailingAnchor],
        [version.bottomAnchor constraintEqualToAnchor:
            background.layoutMarginsGuide.bottomAnchor]
    ]];
    [(UITableViewController *)self tableView].backgroundView = background;
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return 1;
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return 3;
}
- (UITableViewCell *)tableView:(UITableView *)tableView
        cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[[UITableViewCell alloc]
        initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil] autorelease];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    if (indexPath.row == 0) {
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
        NSInteger selected = selectedFPS();
        [button addTarget:self action:@selector(showFPSMenu:)
           forControlEvents:UIControlEventTouchUpInside];
        [button setTitle:[NSString stringWithFormat:@"%ld FPS  ▾", (long)selected]
            forState:UIControlStateNormal];
        [button sizeToFit];
        cell.textLabel.text = @"Frame Rate";
        cell.accessoryView = button;
    } else if (indexPath.row == 1){
        UISwitch *toggle = [[[UISwitch alloc] init] autorelease];
        toggle.on = inputEdgeFixEnabled();
        [toggle addTarget:self action:@selector(setInputEdgeFix:)
            forControlEvents:UIControlEventValueChanged];
        cell.textLabel.text = @"Input Edge Fix";
        cell.accessoryView = toggle;
    }
    else{
        UISwitch *toggle = [[[UISwitch alloc] init] autorelease];
        toggle.on = analyzerEnabled();
        [toggle addTarget:self action:@selector(setAnalyzer:)
            forControlEvents:UIControlEventValueChanged];
        cell.textLabel.text = @"Enable JubeatAnalyzer (Restart the game to take effect)";
        cell.accessoryView = toggle;
    }
    return cell;
}
%new
- (void)setAnalyzer:(UISwitch *)sender {
    [[NSUserDefaults standardUserDefaults] setBool:sender.on forKey:ANALYZER_KEY];
}
%new
- (void)setInputEdgeFix:(UISwitch *)sender {
    [[NSUserDefaults standardUserDefaults] setBool:sender.on forKey:INPUT_FIX_KEY];
    pendingButtonDown = 0;
}
%new
- (void)showFPSMenu:(UIButton *)sender {
    UIAlertController *menu = [UIAlertController alertControllerWithTitle:nil message:nil
        preferredStyle:UIAlertControllerStyleActionSheet];
    NSInteger selected = selectedFPS();
    NSArray *rates = [UIScreen mainScreen].maximumFramesPerSecond >= 120
        ? @[@30, @60, @120] : @[@30, @60];
    for (NSNumber *rate in rates) {
        NSString *title = [NSString stringWithFormat:@"%@%@ FPS",
            selected == rate.integerValue ? @"✓ " : @"", rate];
        [menu addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault
            handler:^(UIAlertAction *action) {
                [[NSUserDefaults standardUserDefaults] setInteger:rate.integerValue forKey:FPS_KEY];
                [sender setTitle:[NSString stringWithFormat:@"%@ FPS  ▾", rate]
                    forState:UIControlStateNormal];
                [sender sizeToFit];
            }]];
    }
    [menu addAction:[UIAlertAction actionWithTitle:@"Cancel"
        style:UIAlertActionStyleCancel handler:nil]];
    menu.popoverPresentationController.sourceView = sender;
    menu.popoverPresentationController.sourceRect = sender.bounds;
    [self presentViewController:menu animated:YES completion:nil];
}
%new
- (void)closeSettings {
    [self dismissViewControllerAnimated:YES completion:nil];
}
%end
%end
#pragma endregion
%group JubeatAnalyzer 
@interface GameViewController : UIViewController
@property (retain, nonatomic) JBTJudgeView* judgeView;
@property (retain, nonatomic) JBTInstantJudgeView* instantJudgeView;
@end
static std::unique_ptr<jbt::Analyzer> analyzer;
template <typename T>
static bool ReadIvar(id object, const char *name, T &value)
{
    if (!object)
        return false;
    Ivar ivar = class_getInstanceVariable(object_getClass(object), name);
    if (!ivar)
    {
        NSLog(@"[JubeatAnalyzer] Missing ivar: %s", name);
        return false;
    }
    auto *base =
        reinterpret_cast<uint8_t *>(object);
    value =
        *reinterpret_cast<T *>(
            base + ivar_getOffset(ivar));
    return true;
}
static std::vector<jbt::SequenceEvent>
SnapshotEvents(Sequence *sequence)
{
    uint32_t numEvent = 0;
    jbt::SequenceEvent *events = nullptr;
    if (!ReadIvar(sequence, "numEvent", numEvent))
        return {};
    if (!ReadIvar(sequence, "events", events))
        return {};
    if (!events || !numEvent)
        return {};
    return std::vector<jbt::SequenceEvent>(
        events,
        events + numEvent);
}
static uint32_t
GetCurrentSector(Sequence *sequence)
{
    uint32_t sector = 0;
    ReadIvar(
        sequence,
        "_currentSector",
        sector);
    return sector;
}
static void StartAnalyzer(Sequence *sequence)
{
    if (!sequence)
    {
        analyzer.reset();
        return;
    }

    auto events =
        SnapshotEvents(sequence);

    if (events.empty())
    {
        NSLog(@"[JubeatAnalyzer] Sequence contains no events");

        analyzer.reset();
        return;
    }

    analyzer.reset(
        new jbt::Analyzer(std::move(events)));

    const auto &stats =
        analyzer->GetStatistics();

    NSLog(
        @"[JubeatAnalyzer] Start "
         "PLAY=%u LONG=%u END=%u MEASURE=%u HAKU=%u TEMPO=%u",
        stats.eventCount[jbt::EVENT_PLAY],
        stats.eventCount[jbt::EVENT_LONG],
        stats.eventCount[jbt::EVENT_END],
        stats.eventCount[jbt::EVENT_MEASURE],
        stats.eventCount[jbt::EVENT_HAKU],
        stats.eventCount[jbt::EVENT_TEMPO]);
}
static void FinishAnalyzer()
{
    if (!analyzer)
        return;
    analyzer.reset();
}
struct JBTInputTouch {
  uint32_t panel;
  CGPoint point;
  UITouchPhase phase;
};

static uint32_t GetPanelsForTouch(GameViewController *controller, id renderer,
                                  CGPoint pointInGLView) {
  bool isPad = false;
  float displayScale = 1.0f;
  float buttonTouchWidth = 0.0f;

  if (!ReadIvar(controller, "isPad", isPad))
    return 0;

  if (!ReadIvar(controller, "displayScale", displayScale)) {
    return 0;
  }

  if (!ReadIvar(controller, "buttonTouchWidth", buttonTouchWidth)) {
    return 0;
  }

  if (displayScale <= 0.0f)
    displayScale = 1.0f;

  //
  // Mirror the coordinate normalization used by the original loop.
  //
  const CGPoint point = CGPointMake(pointInGLView.x / displayScale,
                                    pointInGLView.y / displayScale);

  int buttonMargin = 0;

  if (!isPad) {
    Class delegateClass = NSClassFromString(@"JubeatAppDelegate");

    if (delegateClass) {
      id appDelegate = ((id (*)(id, SEL))objc_msgSend)((id)delegateClass,
                                                       @selector(appDelegate));

      if (appDelegate) {
        const BOOL is4InchAspect = ((BOOL (*)(id, SEL))objc_msgSend)(
            appDelegate, @selector(is4inchAspect));

        if (is4InchAspect) {
          buttonMargin = ((int (*)(id, SEL))objc_msgSend)(
              renderer, @selector(buttonMarginForScreen40));
        }
      }
    }
  }

  uint32_t result = 0;

  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t column = i & 3;
    const uint32_t row = i >> 2;

    CGRect rect;

    if (isPad) {
      rect =
          CGRectMake(192.0 * column + 8.0, 192.0 * row + 264.0, 176.0, 176.0);
    } else {
      rect = CGRectMake(80.0 * column - buttonTouchWidth,
                        buttonMargin + 80.0 * row - buttonTouchWidth + 160.0,
                        buttonTouchWidth * 2.0 + 80.0,
                        buttonTouchWidth * 2.0 + 80.0);
    }

    //
    // Keep all matching panels. The original hit areas may overlap.
    //
    if (CGRectContainsPoint(rect, point))
      result |= 1u << i;
  }

  return result;
}

static std::vector<JBTInputTouch>
CaptureInputTouches(GameViewController *controller, id renderer,
                    JBTInstantJudgeView *overlay) {
  std::vector<JBTInputTouch> result;

  if (!overlay)
    return result;

  UIView *glView = [controller glView];

  if (!glView)
    return result;

  id touches = ((id (*)(id, SEL))objc_msgSend)(glView, @selector(touches));

  if (!touches)
    return result;

  for (UITouch *touch in touches) {
    if (touch.phase == UITouchPhaseEnded ||
        touch.phase == UITouchPhaseCancelled) {
      continue;
    }

    const CGPoint pointInGLView = [touch locationInView:glView];

    const uint32_t panels =
        GetPanelsForTouch(controller, renderer, pointInGLView);

    if (!panels)
      continue;

    const CGPoint pointInOverlay = [glView convertPoint:pointInGLView
                                                 toView:overlay];

    for (uint32_t panel = 0; panel < 16; ++panel) {
      if ((panels & (1u << panel)) == 0)
        continue;

      result.push_back({panel, pointInOverlay, touch.phase});
    }
  }

  return result;
}

static const JBTInputTouch *
FindTouchForPanel(const std::vector<JBTInputTouch> &touches, uint32_t panel) {
  const JBTInputTouch *fallback = nullptr;

  for (const auto &touch : touches) {
    if (touch.panel != panel)
      continue;

    //
    // Prefer a physical touch that actually began this frame.
    //
    if (touch.phase == UITouchPhaseBegan)
      return &touch;

    if (!fallback)
      fallback = &touch;
  }

  return fallback;
}

static bool
FindNearestUnjudgedHead(const std::vector<jbt::SequenceEvent> &events,
                        uint32_t panel, uint32_t currentSector,
                        int32_t &errorSector) {
  bool found = false;
  uint32_t bestDistance = UINT32_MAX;

  for (const auto &event : events) {
    bool candidate = false;

    if (event.type == jbt::EVENT_PLAY) {
      candidate = event.judge == jbt::JUDGE_NONE;
    } else if (event.type == jbt::EVENT_LONG) {
      candidate = event.holdHeadJudge == jbt::JUDGE_NONE;
    }

    if (!candidate)
      continue;

    if ((event.eve.raw & 0xF) != panel)
      continue;

    const int32_t error = static_cast<int32_t>(currentSector) -
                          static_cast<int32_t>(event.sector);

    const uint32_t distance = error < 0 ? static_cast<uint32_t>(-error)
                                        : static_cast<uint32_t>(error);

    if (distance >= bestDistance)
      continue;

    bestDistance = distance;
    errorSector = error;
    found = true;
  }

  return found;
}

static const jbt::JudgeRecord *
FindImmediateJudge(const std::vector<jbt::JudgeRecord> &judges,
                   const std::vector<bool> &used, uint32_t panel) {
  const jbt::JudgeRecord *best = nullptr;
  uint32_t bestDistance = UINT32_MAX;

  for (size_t i = 0; i < judges.size(); ++i) {
    if (used[i])
      continue;

    const auto &judge = judges[i];

    //
    // A button-down only corresponds to PLAY or LONG head.
    //
    if (judge.release)
      continue;

    if (judge.panel != panel)
      continue;

    if (judge.type != jbt::EVENT_PLAY && judge.type != jbt::EVENT_LONG) {
      continue;
    }

    const uint32_t distance = judge.errorSector < 0
                                  ? static_cast<uint32_t>(-judge.errorSector)
                                  : static_cast<uint32_t>(judge.errorSector);

    if (distance >= bestDistance)
      continue;

    best = &judge;
    bestDistance = distance;
  }

  return best;
}

static void ShowInputJudges(JBTInstantJudgeView *overlay,
                            const std::vector<JBTInputTouch> &touches,
                            uint32_t buttonDown, uint32_t judgeSector,
                            const std::vector<jbt::SequenceEvent> &before,
                            const std::vector<jbt::JudgeRecord> &judges) {
  if (!overlay || !buttonDown)
    return;

  std::vector<bool> usedJudges(judges.size(), false);

  for (uint32_t panel = 0; panel < 16; ++panel) {
    if ((buttonDown & (1u << panel)) == 0)
      continue;

    const JBTInputTouch *touch = FindTouchForPanel(touches, panel);

    //
    // Auto/replay inputs do not have a real UITouch position.
    //
    if (!touch)
      continue;

    NSInteger result = jbt::JUDGE_NONE;

    double milliseconds = NAN;

    const jbt::JudgeRecord *judge =
        FindImmediateJudge(judges, usedJudges, panel);

    if (judge) {
      result = judge->result;

      milliseconds = static_cast<double>(judge->errorSector) * 1000.0 / 300.0;
      analyzer->RecordPressTiming(judge->index,judge->errorSector);

      const size_t judgeIndex = static_cast<size_t>(judge - judges.data());

      usedJudges[judgeIndex] = true;
    } else {
      //
      // The user pressed a panel but the game did not resolve a judge
      // this frame, e.g. an input outside the judgement window.
      //
      int32_t errorSector = 0;

      if (FindNearestUnjudgedHead(before, panel, judgeSector, errorSector)) {
        milliseconds = static_cast<double>(errorSector) * 1000.0 / 300.0;
      }
    }

    [overlay showJudge:result
        timingOffsetMilliseconds:milliseconds
                         atPoint:touch->point];
  }
}

%hook GameViewController
%property (retain, nonatomic) JBTJudgeView* judgeView;
%property (retain, nonatomic) JBTInstantJudgeView* instantJudgeView;
- (void)loadResources{
    %orig;
    UIView* autoSwitchView = nil; // This seems to be cover
    if (!ReadIvar(self, "autoSwitch", autoSwitchView))
        return;
    self.judgeView = [[JBTJudgeView alloc] initWithFrame:autoSwitchView.bounds];
    self.judgeView.userInteractionEnabled = NO;
    [autoSwitchView addSubview:self.judgeView];
    [autoSwitchView bringSubviewToFront:self.judgeView];
    self.instantJudgeView =
        [[JBTInstantJudgeView alloc]
            initWithFrame:self.view.bounds];

    self.instantJudgeView.autoresizingMask =
        UIViewAutoresizingFlexibleWidth |
        UIViewAutoresizingFlexibleHeight;
    self.instantJudgeView.userInteractionEnabled = NO;

    [self.view addSubview:self.instantJudgeView];
    [self.view bringSubviewToFront:self.instantJudgeView];
}
- (void)startGame{
    %orig;
    Sequence* seq = [self performSelector:@selector(sequence)];
    StartAnalyzer(seq);
    [self.instantJudgeView clear];
}
- (void)restartGame{
    %orig;
    Sequence* seq = [self performSelector:@selector(sequence)];
    StartAnalyzer(seq);
    [self.instantJudgeView clear];
}
- (void)end{
    FinishAnalyzer();
    [self.instantJudgeView clear];
    %orig;
}
- (void)loop:(id)sender
{
    //
    // Only state 3 executes Sequence::judge:btnPress:.
    //
    if (!analyzer)
    {
        %orig;
        return;
    }

    id renderer =
        [self mainGameRenderer];

    const unsigned int state =
        [renderer state];

    if (state != 3)
    {
        %orig;
        return;
    }

    Sequence *sequence =
        [self sequence];

    if (!sequence)
    {
        %orig;
        return;
    }


    //
    // Capture the actual finger positions before the original loop consumes
    // the current touch state.
    //
    const auto inputTouches = CaptureInputTouches(self, renderer, self.instantJudgeView);

    //
    // IMPORTANT:
    //
    // The original loop calls Sequence::judge BEFORE seekToTime:.
    // Therefore this is the exact sector seen by judge:btnPress:.
    //
    const uint32_t judgeSector =
        GetCurrentSector(sequence);


    //
    // Capture the event runtime state before judgement.
    //
    auto before =
        SnapshotEvents(sequence);

    if (before.empty())
    {
        %orig;
        return;
    }

    analyzer->BeforeJudge(before);


    //
    // Original loop:
    //
    //   input calculation
    //   Sequence::judge(...)
    //   Sequence::seekToTime(...)
    //   rendering...
    //
    %orig;


    //
    // saveScore / restart etc. could theoretically destroy the analyzer from
    // inside %orig. Do not access it afterwards if that happened.
    //
    if (!analyzer)
        return;


    //
    // Capture the event runtime state after judgement.
    //
    auto after =
        SnapshotEvents(sequence);

    if (after.empty())
        return;


    //
    // Use judgeSector, NOT the current sector after %orig.
    //
    // seekToTime: has already advanced _currentSector by this point.
    //
    analyzer->AfterJudge(
        after,
        judgeSector);
    const auto &stats = analyzer->GetStatistics();
    double songAverageMS = analyzer->GetSongAverageTimingMs();
    double regionAverageMS = analyzer->GetCurrentRegionAverageTimingMs(judgeSector);
    self.judgeView.perfect = stats.perfect;
    self.judgeView.great = stats.great;
    self.judgeView.good = stats.good;
    self.judgeView.poor = stats.poor;
    self.judgeView.miss = stats.miss;
    self.judgeView.songAverageMS = songAverageMS;
    self.judgeView.regionAverageMS = regionAverageMS;
    //
    // %orig has now calculated this frame's buttonDown mask.
    //
    uint32_t buttonDown = 0;

    if (ReadIvar(
            self,
            "buttonDown",
            buttonDown))
    {
        ShowInputJudges(
            self.instantJudgeView,
            inputTouches,
            buttonDown,
            judgeSector,
            before,
            analyzer->GetRecentJudges());
    }
}
%end
%end

extern "C" void init_Jubeat(void){
    %init(Jubeat);
    // Set MarkerInfo
    NSString *path = [[getDocumentsPath() stringByAppendingPathComponent:@"marker"] stringByAppendingPathComponent:@"marker-list.plist"];
    if([[NSFileManager defaultManager] fileExistsAtPath:path]){
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (!data)
            return;

        NSError *error = nil;
        id markerList = [NSPropertyListSerialization propertyListWithData:data
                                                                options:NSPropertyListImmutable
                                                                    format:NULL
                                                                    error:&error];
        if (error){
            NSLog(@"[BemaniTools] Deserializing marker list failed: %@", error);
            return;
        }
        [objc_getClass("MarkerManager") setMarkerList:markerList];
        [[NSUserDefaults standardUserDefaults] synchronize];
        [markerList release];
    }
    %init(JubeatAnalyzer);
}
