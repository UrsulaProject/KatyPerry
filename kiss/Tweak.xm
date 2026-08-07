#include "utils.h"
%ctor{
        if(objc_getClass("JubeatAppDelegate")){
            init_Jubeat();
        }
        else if(objc_getClass("RBPurchaseManager")){
            init_ReflecBeat();
        }
}