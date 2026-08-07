%config(generator=internal)
#include <objc/runtime.h>
#include <UIKit/UIKit.h>
#include <QuartzCore/QuartzCore.h>
#include "fishhook.h"
#include "utils.h"
#define MULIST_KEY @"SHARED_KEY"
#define FPS_KEY @"KISS_FORCED_FPS"
#define INPUT_FIX_KEY @"KISS_INPUT_EDGE_FIX"
#define RB_CUSTOM_THEME_KEY @"KISS_RB_CUSTOM_THEME"
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


extern "C" void init_ReflecBeat(void){
    NSLog(@"Initializing ReflecBeat Hooks");
    InstallDirectoryRedirect();
    RBValidateConfiguredCustomTheme();
    %init(ReflecBeat);
}
