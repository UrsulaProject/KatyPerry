%config(generator=internal)
#include <objc/runtime.h>
#include <Foundation/Foundation.h>
#include <UIKit/UIKit.h>
#include <QuartzCore/QuartzCore.h>
#define MULIST_KEY @"SHARED_KEY"
#define MAXIMUM_FPS_KEY @"maxfps_enable"

static NSString* getDocumentsPath(){
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return [paths objectAtIndex:0];
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
    UISwitch *toggle = [[[UISwitch alloc] initWithFrame:CGRectMake(0, 0, 0, 0)] autorelease];
    cell.textLabel.text = @"Maximum FPS";
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    toggle.on = [[NSUserDefaults standardUserDefaults] boolForKey:MAXIMUM_FPS_KEY];
    [toggle addTarget:self action:@selector(toggleMaximumFPS:)
       forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = toggle;
    return cell;
}
%new
- (void)toggleMaximumFPS:(UISwitch *)sender {
    [[NSUserDefaults standardUserDefaults] setBool:sender.on forKey:MAXIMUM_FPS_KEY];
    [[NSUserDefaults standardUserDefaults] synchronize];
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
    BOOL enabled = [[NSUserDefaults standardUserDefaults] boolForKey:MAXIMUM_FPS_KEY];
    NSInteger maximum = [UIScreen mainScreen].maximumFramesPerSecond;
    if (@available(iOS 15.0, *))
        link.preferredFrameRateRange = enabled && maximum > 60
            ? CAFrameRateRangeMake(maximum, maximum, maximum)
            : CAFrameRateRangeMake(enabled ? 60 : 30, enabled ? 60 : 30, enabled ? 60 : 30);
    else
        link.preferredFramesPerSecond = enabled ? maximum : 30;
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
