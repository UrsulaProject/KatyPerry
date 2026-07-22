%config(generator=internal)
#include <objc/runtime.h>
#include <Foundation/Foundation.h>
#define MULIST_KEY @"SHARED_KEY"
static NSString* getDocumentsPath(){
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return [paths objectAtIndex:0];
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
%end


%ctor{
    if(objc_getClass("JubeatAppDelegate")){
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