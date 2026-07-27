%config(generator=internal)
#include <objc/runtime.h>
#include <Foundation/Foundation.h>
#include <UIKit/UIKit.h>
#include <QuartzCore/QuartzCore.h>
#define MULIST_KEY @"SHARED_KEY"
#define FPS_KEY @"KISS_FORCED_FPS"
#define INPUT_FIX_KEY @"KISS_INPUT_EDGE_FIX"

static id activeGameController;
static unsigned int pendingButtonDown;

static NSString* getDocumentsPath(){
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return [paths objectAtIndex:0];
}

static NSInteger selectedFPS(){
    NSInteger fps = [[NSUserDefaults standardUserDefaults] integerForKey:FPS_KEY];
    return fps == 60 || (fps == 120 && [UIScreen mainScreen].maximumFramesPerSecond >= 120) ? fps : 30;
}

static BOOL inputEdgeFixEnabled(){
    id value = [[NSUserDefaults standardUserDefaults] objectForKey:INPUT_FIX_KEY];
    return value ? [value boolValue] : YES;
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

%subclass BemaniSettingsViewController : UITableViewController
- (void)viewDidLoad {
    %orig;
    [(UITableViewController *)self setTitle:@"KISS"];
    [(UITableViewController *)self navigationItem].leftBarButtonItem = [[[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemDone
        target:self
        action:@selector(closeSettings)] autorelease];
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return 1;
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return 2;
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
    } else {
        UISwitch *toggle = [[[UISwitch alloc] init] autorelease];
        toggle.on = inputEdgeFixEnabled();
        [toggle addTarget:self action:@selector(setInputEdgeFix:)
            forControlEvents:UIControlEventValueChanged];
        cell.textLabel.text = @"Input Edge Fix";
        cell.accessoryView = toggle;
    }
    return cell;
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
%end


%ctor{
    if(objc_getClass("JubeatAppDelegate")){
        %init(_ungrouped);
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
    }  
}
