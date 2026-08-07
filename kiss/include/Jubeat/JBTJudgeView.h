// JBTJudgeView.h

#import <UIKit/UIKit.h>

@interface JBTJudgeView : UIView

@property(nonatomic, assign) NSInteger perfect;
@property(nonatomic, assign) NSInteger great;
@property(nonatomic, assign) NSInteger good;
@property(nonatomic, assign) NSInteger poor;
@property(nonatomic, assign) NSInteger miss;
@property(nonatomic, assign) double songAverageMS;
@property(nonatomic, assign) double regionAverageMS;

@end