%config(generator=internal)
#include <objc/runtime.h>
#include <Foundation/Foundation.h>
#include <UIKit/UIKit.h>
#include <QuartzCore/QuartzCore.h>
#include "fishhook.h"
#define MULIST_KEY @"SHARED_KEY"
#define FPS_KEY @"KISS_FORCED_FPS"
#define INPUT_FIX_KEY @"KISS_INPUT_EDGE_FIX"
#define RB_CUSTOM_THEME_KEY @"KISS_RB_CUSTOM_THEME"


static NSString* getDocumentsPath(){
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return [paths objectAtIndex:0];
}
// DirectoryRedirect.m

#import <Foundation/Foundation.h>
#import "fishhook.h"

typedef NSArray<NSString *> *(*NSSearchPathFn)(
    NSSearchPathDirectory directory,
    NSSearchPathDomainMask domainMask,
    BOOL expandTilde
);

static NSSearchPathFn original_NSSearchPathForDirectoriesInDomains = NULL;

static NSArray<NSString *> *
replaced_NSSearchPathForDirectoriesInDomains(
    NSSearchPathDirectory directory,
    NSSearchPathDomainMask domainMask,
    BOOL expandTilde)
{
    NSFileManager* fm = [NSFileManager defaultManager];
    switch(directory){
        case NSLibraryDirectory:{
            NSString* dir = [getDocumentsPath() stringByAppendingPathComponent:@"Library"];
            if(![fm fileExistsAtPath:dir]){
                [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            }
            return @[dir];
        }
        case NSCachesDirectory:{
            NSString* dir = [getDocumentsPath() stringByAppendingPathComponent:@"Caches"];
            if(![fm fileExistsAtPath:dir]){
                [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            }
            return @[dir];
        }
        default:{
            break;
        }
    }
    return original_NSSearchPathForDirectoriesInDomains(
            directory,
            domainMask,
            expandTilde
    );
}

void InstallDirectoryRedirect(void)
{
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        struct rebinding binding = {
            .name = "NSSearchPathForDirectoriesInDomains",
            .replacement =
                (void *)replaced_NSSearchPathForDirectoriesInDomains,
            .replaced =
                (void **)&original_NSSearchPathForDirectoriesInDomains,
        };

        int result = rebind_symbols(&binding, 1);
        NSLog(@"[KISS] fishhook rebind result: %d", result);
    });
}

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
%end
#pragma endregion

@interface RBUserSettingData : NSObject
+ (instancetype)sharedInstance;
- (int)thema;
- (void)setThema:(int)thema;
- (void)save;
@end

@interface RBThemaView : UIView
- (UIScrollView *)scrollView;
- (UIButton *)okButton;
- (int)thema;
- (void)setThema:(int)thema;
@end

static char RBExtraThemeNamesKey;
static char RBSelectedCustomThemeNameKey;

static NSArray *RBBuiltInThemeNames(void) {
    return @[@"01_Classic", @"02_Limelight", @"03_Colette"];
}

static NSString *RBDeviceImageDirectoryName(void) {
    BOOL isPad = UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad;
    BOOL isRetina = [UIScreen mainScreen].scale != 1.0;
    if (isPad)
        return isRetina ? @"iPad2x" : @"iPad";
    return isRetina ? @"iPhone@2x" : @"iPhone";
}

static NSString *RBThemeRootDirectory(void) {
    NSString *library = [NSSearchPathForDirectoriesInDomains(
        NSLibraryDirectory, NSUserDomainMask, YES) lastObject];
    NSString *images = [[library stringByAppendingPathComponent:@"Private Documents"]
        stringByAppendingPathComponent:@"Images"];
    return [images stringByAppendingPathComponent:RBDeviceImageDirectoryName()];
}

static NSArray *RBExtraThemes(void) {
    NSString *path = [getDocumentsPath()
        stringByAppendingPathComponent:@"ExtraThemes.plist"];
    NSArray *plist = [NSArray arrayWithContentsOfFile:path];
    if (![plist isKindOfClass:[NSArray class]])
        return @[];

    NSMutableArray *result = [NSMutableArray array];
    NSMutableArray *themeNames = [NSMutableArray array];
    for (id value in plist) {
        if (![value isKindOfClass:[NSDictionary class]])
            continue;
        NSString *themeName = [(NSDictionary *)value objectForKey:@"theme"];
        NSString *coverName = [(NSDictionary *)value objectForKey:@"cover"];
        if (![themeName isKindOfClass:[NSString class]] ||
            ![coverName isKindOfClass:[NSString class]] ||
            !themeName.length || !coverName.length ||
            ![themeName isEqualToString:[themeName lastPathComponent]] ||
            ![coverName isEqualToString:[coverName lastPathComponent]] ||
            [coverName pathExtension].length ||
            [themeName isEqualToString:@"00_Share"] ||
            [RBBuiltInThemeNames() containsObject:themeName] ||
            [themeNames containsObject:themeName])
            continue;
        [themeNames addObject:themeName];
        [result addObject:@{@"theme": themeName, @"cover": coverName}];
    }
    return result;
}

static NSArray *RBExtraThemeNames(void) {
    NSMutableArray *result = [NSMutableArray array];
    for (NSDictionary *theme in RBExtraThemes())
        [result addObject:[theme objectForKey:@"theme"]];
    return result;
}

static NSString *RBConfiguredCustomThemeName(void) {
    NSString *name = [[NSUserDefaults standardUserDefaults]
        stringForKey:RB_CUSTOM_THEME_KEY];
    if (!name.length || [RBBuiltInThemeNames() containsObject:name])
        return nil;
    return name;
}

static NSString *RBThemeCoverPath(NSString *themeName, NSString *coverName) {
    NSString *fileName = [coverName stringByAppendingPathExtension:@"png"];
    NSString *root = RBThemeRootDirectory();
    NSArray *directories = @[
        [[root stringByAppendingPathComponent:@"00_Share"]
            stringByAppendingPathComponent:@"05_theme"],
        [[root stringByAppendingPathComponent:themeName]
            stringByAppendingPathComponent:@"05_theme"]
    ];
    for (NSString *directory in directories) {
        NSString *path = [directory stringByAppendingPathComponent:fileName];
        if ([[NSFileManager defaultManager] fileExistsAtPath:path])
            return path;
    }
    return nil;
}

static void RBValidateConfiguredCustomTheme(void) {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    NSString *name = [defaults stringForKey:RB_CUSTOM_THEME_KEY];
    if (name.length && ![RBExtraThemeNames() containsObject:name])
        [defaults removeObjectForKey:RB_CUSTOM_THEME_KEY];
}

%group ReflecBeat
%hook AppDelegate
+(NSString*)musicListKey{
    return MULIST_KEY;
}
%end

%hook RBPurchaseManager
- (BOOL)isPurchased:(id)arg1 {
    return YES;
}
%end

%hook RBExperienceData
- (BOOL)unlockWithThemaID:(int)arg1 {
    return YES;
}
- (BOOL)unlockWithMusicID:(int)arg1 {
    return YES;
}
- (BOOL)unlockWithBackgroundType:(int)arg1 {
    return YES;
}
- (BOOL)unlockWithFrameType:(int)arg1 {
    return YES;
}
- (BOOL)unlockWithExprosionType:(int)arg1 {
    return YES;
}
- (BOOL)unlockWithShotType:(int)arg1 {
    return YES;
}
- (BOOL)unlockWithBGMtype:(int)arg1 {
    return YES;
}
- (float)getPoint {
    return 999999;
}
%end

%hook RBTutorialManager
+ (BOOL)needStartTutorialStore {
    return NO;
}
+ (BOOL)needStartTutorialCustomize {
    return NO;
}
+ (BOOL)needStartTutorialPlay {
    return NO;
}
+ (BOOL)needStartTutorialMusicselect {
    return NO;
}
+ (BOOL)isTutorial {
    return NO;
}
%end

%hook RBUserSettingData
- (NSString *)themaName {
    NSString *customName = RBConfiguredCustomThemeName();
    return customName ?: %orig;
}
%end

%hook UIImage
+ (UIImage *)imageWithName:(NSString *)name useCache:(BOOL)useCache {
    return %orig(name, RBConfiguredCustomThemeName() ? NO : useCache);
}
%end

%hook RBThemaView
- (void)setupView {
    %orig;

    NSArray *extraThemes = RBExtraThemes();
    if (!extraThemes.count)
        return;

    NSMutableArray *themeNames = [NSMutableArray arrayWithCapacity:
        extraThemes.count];
    for (NSDictionary *theme in extraThemes)
        [themeNames addObject:[theme objectForKey:@"theme"]];
    objc_setAssociatedObject(self, &RBExtraThemeNamesKey, themeNames,
        OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    UIScrollView *scrollView = [self scrollView];
    CGFloat pageWidth = CGRectGetWidth(scrollView.bounds);
    CGFloat pageHeight = CGRectGetHeight(scrollView.bounds);
    for (NSUInteger index = 0; index < extraThemes.count; ++index) {
        NSDictionary *theme = extraThemes[index];
        NSString *coverPath = RBThemeCoverPath(
            [theme objectForKey:@"theme"], [theme objectForKey:@"cover"]);
        UIImage *cover = coverPath
            ? [UIImage imageWithContentsOfFile:coverPath] : nil;
        UIImageView *coverView = [[UIImageView alloc] initWithImage:cover];
        coverView.frame = CGRectMake(pageWidth * (3 + index), 0.0,
            pageWidth, pageHeight);
        coverView.contentMode = UIViewContentModeScaleAspectFit;
        coverView.exclusiveTouch = YES;
        [scrollView addSubview:coverView];
        [coverView release];
    }

    scrollView.contentSize = CGSizeMake(
        pageWidth * (3 + themeNames.count), scrollView.contentSize.height);

    NSString *configuredName = RBConfiguredCustomThemeName();
    NSUInteger configuredIndex = configuredName
        ? [themeNames indexOfObject:configuredName] : NSNotFound;
    if (configuredIndex != NSNotFound) {
        objc_setAssociatedObject(self, &RBSelectedCustomThemeNameKey,
            configuredName, OBJC_ASSOCIATION_COPY_NONATOMIC);
        scrollView.contentOffset = CGPointMake(
            pageWidth * (3 + configuredIndex), 0.0);
        [[self okButton] setEnabled:NO];
    }
}

- (void)scrollViewDidScroll:(UIScrollView *)scrollView {
    %orig;

    NSArray *themeNames = objc_getAssociatedObject(self,
        &RBExtraThemeNamesKey);
    CGFloat pageWidth = CGRectGetWidth(scrollView.bounds);
    if (!themeNames.count || pageWidth <= 0.0)
        return;

    NSInteger page = (NSInteger)(scrollView.contentOffset.x /
        pageWidth + 0.5);
    NSString *selectedCustomName = nil;
    NSInteger customIndex = page - 3;
    if (customIndex >= 0 && customIndex < (NSInteger)themeNames.count)
        selectedCustomName = themeNames[(NSUInteger)customIndex];

    objc_setAssociatedObject(self, &RBSelectedCustomThemeNameKey,
        selectedCustomName, OBJC_ASSOCIATION_COPY_NONATOMIC);

    NSString *configuredName = RBConfiguredCustomThemeName();
    BOOL changed;
    if (selectedCustomName) {
        [self setThema:2];
        changed = ![selectedCustomName isEqualToString:configuredName];
    } else {
        RBUserSettingData *settings =
            [(id)objc_getClass("RBUserSettingData") sharedInstance];
        changed = configuredName != nil || [self thema] != [settings thema];
    }
    [[self okButton] setEnabled:changed];
}

- (void)yesButtonTouch:(id)sender {
    NSArray *themeNames = objc_getAssociatedObject(self,
        &RBExtraThemeNamesKey);
    if (!themeNames.count) {
        %orig;
        return;
    }

    NSString *selectedCustomName = objc_getAssociatedObject(self,
        &RBSelectedCustomThemeNameKey);
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    if (selectedCustomName) {
        [defaults setObject:selectedCustomName forKey:RB_CUSTOM_THEME_KEY];
        [self setThema:2];
    } else {
        [defaults removeObjectForKey:RB_CUSTOM_THEME_KEY];
    }
    [defaults synchronize];
    %orig;
}
%end
%end


%ctor{
    if(objc_getClass("JubeatAppDelegate")){
        NSLog(@"Initializing Jubeat Hooks");
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
    else if(objc_getClass("RBPurchaseManager")){
        NSLog(@"Initializing ReflecBeat Hooks");
        InstallDirectoryRedirect();
        RBValidateConfiguredCustomTheme();
        %init(ReflecBeat);
    }
}
