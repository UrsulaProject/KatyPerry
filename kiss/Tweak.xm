%config(generator=internal)
#include <objc/runtime.h>
#include <Foundation/Foundation.h>
#include <UIKit/UIKit.h>
#include <QuartzCore/QuartzCore.h>
#define MULIST_KEY @"SHARED_KEY"
#define FPS_KEY @"KISS_FORCED_FPS"

static NSString* getDocumentsPath(){
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return [paths objectAtIndex:0];
}

static NSInteger selectedFPS(){
    NSInteger fps = [[NSUserDefaults standardUserDefaults] integerForKey:FPS_KEY];
    return fps == 60 || (fps == 120 && [UIScreen mainScreen].maximumFramesPerSecond >= 120) ? fps : 30;
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
    return 1;
}
- (UITableViewCell *)tableView:(UITableView *)tableView
        cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[[UITableViewCell alloc]
        initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil] autorelease];
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    NSInteger selected = selectedFPS();
    [button addTarget:self action:@selector(showFPSMenu:)
       forControlEvents:UIControlEventTouchUpInside];
    [button setTitle:[NSString stringWithFormat:@"%ld FPS  ▾", (long)selected]
        forState:UIControlStateNormal];
    [button sizeToFit];
    cell.textLabel.text = @"Frame Rate";
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.accessoryView = button;
    return cell;
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
