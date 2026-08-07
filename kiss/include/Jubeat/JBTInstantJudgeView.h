#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface JBTInstantJudgeView : UIView

- (void)showJudge:(NSInteger)judge
timingOffsetMilliseconds:(double)milliseconds
           atPoint:(CGPoint)point;

- (void)clear;

@end

NS_ASSUME_NONNULL_END
