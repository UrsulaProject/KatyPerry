#import "Jubeat/JBTInstantJudgeView.h"

#import <QuartzCore/QuartzCore.h>
#import <math.h>

static NSString *JBTJudgeName(NSInteger judge)
{
    switch (judge)
    {
        case 1:
            return @"MISS";
        case 2:
            return @"POOR";
        case 3:
            return @"GOOD";
        case 4:
            return @"GREAT";
        case 5:
            return @"PERFECT";
        default:
            return @"NO JUDGE";
    }
}

static UIColor *JBTJudgeColor(NSInteger judge)
{
    switch (judge)
    {
        case 1:
            return [UIColor colorWithRed:1.00
                                   green:0.18
                                    blue:0.18
                                   alpha:1.0];

        case 2:
            return [UIColor colorWithRed:1.00
                                   green:0.48
                                    blue:0.12
                                   alpha:1.0];

        case 3:
            return [UIColor colorWithRed:1.00
                                   green:0.85
                                    blue:0.15
                                   alpha:1.0];

        case 4:
            return [UIColor colorWithRed:0.20
                                   green:0.85
                                    blue:1.00
                                   alpha:1.0];

        case 5:
            return [UIColor colorWithRed:1.00
                                   green:0.20
                                    blue:0.75
                                   alpha:1.0];

        default:
            return [UIColor colorWithWhite:0.85
                                     alpha:1.0];
    }
}

@implementation JBTInstantJudgeView

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self)
    {
        self.userInteractionEnabled = NO;
        self.backgroundColor = UIColor.clearColor;
        self.clipsToBounds = NO;
    }

    return self;
}

- (void)showJudge:(NSInteger)judge
timingOffsetMilliseconds:(double)milliseconds
           atPoint:(CGPoint)point
{
    static const CGFloat containerWidth = 150.0;
    static const CGFloat containerHeight = 76.0;
    static const CGFloat anchorX = containerWidth * 0.5;
    static const CGFloat anchorY = 58.0;

    UIColor *color = JBTJudgeColor(judge);

    UIView *container =
        [[UIView alloc] initWithFrame:CGRectMake(
            point.x - anchorX,
            point.y - anchorY,
            containerWidth,
            containerHeight)];

    container.userInteractionEnabled = NO;
    container.backgroundColor = UIColor.clearColor;

    UIView *ring =
        [[UIView alloc] initWithFrame:CGRectMake(
            anchorX - 17.0,
            anchorY - 17.0,
            34.0,
            34.0)];

    ring.userInteractionEnabled = NO;
    ring.backgroundColor =
        [color colorWithAlphaComponent:0.12];
    ring.layer.borderColor = color.CGColor;
    ring.layer.borderWidth = 2.0;
    ring.layer.cornerRadius = 17.0;

    UIView *dot =
        [[UIView alloc] initWithFrame:CGRectMake(
            anchorX - 3.0,
            anchorY - 3.0,
            6.0,
            6.0)];

    dot.userInteractionEnabled = NO;
    dot.backgroundColor = color;
    dot.layer.cornerRadius = 3.0;

    UILabel *label =
        [[UILabel alloc] initWithFrame:CGRectMake(
            0.0,
            0.0,
            containerWidth,
            36.0)];

    label.userInteractionEnabled = NO;
    label.backgroundColor =
        [UIColor colorWithWhite:0.0 alpha:0.58];
    label.textColor = color;
    label.font =
        [UIFont boldSystemFontOfSize:13.0];
    label.textAlignment =
        NSTextAlignmentCenter;
    label.layer.cornerRadius = 7.0;
    label.clipsToBounds = YES;

    if (isnan(milliseconds))
    {
        label.text =
            JBTJudgeName(judge);
    }
    else
    {
        label.text =
            [NSString stringWithFormat:
                @"%@  %+.1f ms",
                JBTJudgeName(judge),
                milliseconds];
    }

    [container addSubview:ring];
    [container addSubview:dot];
    [container addSubview:label];

    [self addSubview:container];

    container.alpha = 0.0;
    container.transform =
        CGAffineTransformMakeScale(0.82, 0.82);

    [UIView animateWithDuration:0.10
                     animations:^{
        container.alpha = 1.0;
        container.transform =
            CGAffineTransformIdentity;
    }
                     completion:^(__unused BOOL finished) {
        [UIView animateWithDuration:0.42
                              delay:0.18
                            options:UIViewAnimationOptionCurveEaseOut
                         animations:^{
            container.alpha = 0.0;
            container.transform =
                CGAffineTransformMakeTranslation(
                    0.0,
                    -14.0);
        }
                         completion:^(__unused BOOL finished2) {
            [container removeFromSuperview];
        }];
    }];
}

- (void)clear
{
    for (UIView *view in self.subviews.copy)
        [view removeFromSuperview];
}

@end