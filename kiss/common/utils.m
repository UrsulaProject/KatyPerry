#include <Foundation/Foundation.h>
#include <objc/runtime.h>
NSString* getDocumentsPath(){
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return [paths objectAtIndex:0];
}