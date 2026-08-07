// JBTJudgeView.m

#import "Jubeat/JBTJudgeView.h"
#import <QuartzCore/QuartzCore.h>

static UIColor *JBTRainbowColorForSize(CGSize size)
{
    if (size.width <= 0.0 || size.height <= 0.0)
        return UIColor.whiteColor;

    UIGraphicsBeginImageContextWithOptions(size, NO, 0.0);

    CGContextRef context = UIGraphicsGetCurrentContext();

    NSArray *colors = @[
        (id)[UIColor colorWithRed:1.00 green:0.15 blue:0.65 alpha:1.0].CGColor,
        (id)[UIColor colorWithRed:1.00 green:0.38 blue:0.35 alpha:1.0].CGColor,
        (id)[UIColor colorWithRed:1.00 green:0.70 blue:0.05 alpha:1.0].CGColor,
        (id)[UIColor colorWithRed:1.00 green:0.95 blue:0.10 alpha:1.0].CGColor,
        (id)[UIColor colorWithRed:0.55 green:0.92 blue:0.15 alpha:1.0].CGColor,
        (id)[UIColor colorWithRed:0.10 green:0.78 blue:0.55 alpha:1.0].CGColor
    ];

    CGFloat locations[] = {
        0.00,
        0.20,
        0.38,
        0.58,
        0.78,
        1.00
    };

    CGColorSpaceRef colorSpace =
        CGColorSpaceCreateDeviceRGB();

    CGGradientRef gradient =
        CGGradientCreateWithColors(
            colorSpace,
            (__bridge CFArrayRef)colors,
            locations
        );

    CGContextDrawLinearGradient(
        context,
        gradient,
        CGPointMake(0.0, size.height * 0.5),
        CGPointMake(size.width, size.height * 0.5),
        0
    );

    UIImage *image =
        UIGraphicsGetImageFromCurrentImageContext();

    CGGradientRelease(gradient);
    CGColorSpaceRelease(colorSpace);

    UIGraphicsEndImageContext();

    return [UIColor colorWithPatternImage:image];
}

static UIFont *JBTTitleFont(CGFloat size)
{
    UIFont *font = [UIFont fontWithName:@"HelveticaNeue-Light" size:size];

    if (!font) {
        font = [UIFont systemFontOfSize:size weight:UIFontWeightLight];
    }

    return font;
}

static UIFont *JBTValueFont(CGFloat size)
{
    UIFont *font = [UIFont fontWithName:@"HelveticaNeue-UltraLight" size:size];

    if (!font) {
        font = [UIFont systemFontOfSize:size weight:UIFontWeightThin];
    }

    return font;
}

static void JBTConfigureCoreLabelGlow(
    UILabel *label,
    UIColor *color
)
{
    //
    // Keep the foreground glyph sharp while adding a small bright halo.
    //
    label.layer.masksToBounds = NO;
    label.layer.shadowColor = color.CGColor;
    label.layer.shadowOffset = CGSizeZero;
    label.layer.shadowOpacity = 0.90;
    label.layer.shadowRadius = 1.0;
}

static void JBTConfigureBloomLabel(
    UILabel *label,
    UIColor *color
)
{
    //
    // A separate label behind the foreground text provides the wide bloom.
    //
    label.textColor =
        [color colorWithAlphaComponent:0.55];

    label.backgroundColor =
        UIColor.clearColor;

    label.userInteractionEnabled = NO;

    label.layer.masksToBounds = NO;
    label.layer.shadowColor = color.CGColor;
    label.layer.shadowOffset = CGSizeZero;
    label.layer.shadowOpacity = 1.0;
    label.layer.shadowRadius = 5.0;
}

static void JBTConfigureLineGlow(
    UIView *line,
    UIColor *color
)
{
    line.layer.masksToBounds = NO;
    line.layer.shadowColor = color.CGColor;
    line.layer.shadowOffset = CGSizeZero;
    line.layer.shadowOpacity = 0.75;
    line.layer.shadowRadius = 3.0;
}

@interface JBTJudgeView ()

@property(nonatomic, strong) NSArray<UILabel *> *titleLabels;
@property(nonatomic, strong) NSArray<UILabel *> *titleGlowLabels;

@property(nonatomic, strong) NSArray<UILabel *> *valueLabels;
@property(nonatomic, strong) NSArray<UILabel *> *valueGlowLabels;

@property(nonatomic, strong) NSArray<UIView *> *lines;

@property(nonatomic, strong) UIVisualEffectView *blurView;
@property(nonatomic, strong) UIViewPropertyAnimator *blurAnimator;

@property(nonatomic, strong) CAGradientLayer *perfectLineGradient;

@end

@implementation JBTJudgeView

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (!self)
        return nil;

    self.backgroundColor = UIColor.clearColor;
    self.opaque = NO;

    // Add Backgroud Blurry Effect
    UIBlurEffectStyle blurStyle;
    if (@available(iOS 13.0, *)) {
        blurStyle = UIBlurEffectStyleSystemUltraThinMaterial;
    } else {
        blurStyle = UIBlurEffectStyleLight;
    }

    UIBlurEffect *blurEffect =
        [UIBlurEffect effectWithStyle:blurStyle];

    self.blurView =
        [[UIVisualEffectView alloc] initWithEffect:nil];

    self.blurView.userInteractionEnabled = NO;
    [self addSubview:self.blurView];

    self.blurAnimator =
    [[UIViewPropertyAnimator alloc]
        initWithDuration:1.0
                  curve:UIViewAnimationCurveLinear
             animations:^{
                 self.blurView.effect = blurEffect;
             }];

    [self.blurAnimator startAnimation];
    [self.blurAnimator pauseAnimation];
    self.blurAnimator.fractionComplete = 0.30;

    NSArray<NSString *> *titles = @[
        @"PERFECT",
        @"GREAT",
        @"GOOD",
        @"POOR",
        @"MISS",
        @"SONG Timing Offset",
        @"REGION Timing Offset"
    ];

    NSArray<UIColor *> *colors = @[
        [UIColor colorWithRed:1.00 green:0.25 blue:0.70 alpha:1.0],
        [UIColor colorWithRed:1.00 green:0.65 blue:0.00 alpha:1.0],
        [UIColor colorWithRed:0.00 green:0.75 blue:1.00 alpha:1.0],
        [UIColor colorWithRed:1.00 green:0.10 blue:0.50 alpha:1.0],
        [UIColor colorWithWhite:0.82 alpha:1.0],
        [UIColor greenColor],
        [UIColor purpleColor],
    ];

    NSMutableArray *titleLabels =
        [NSMutableArray array];

    NSMutableArray *titleGlowLabels =
        [NSMutableArray array];

    NSMutableArray *valueLabels =
        [NSMutableArray array];

    NSMutableArray *valueGlowLabels =
        [NSMutableArray array];

    NSMutableArray *lines =
        [NSMutableArray array];

    for (NSUInteger i = 0; i < titles.count; ++i) {
        UIColor *color = colors[i];

        //
        // Title bloom.
        //
        UILabel *titleGlowLabel =
            [[UILabel alloc] init];

        titleGlowLabel.text = titles[i];
        titleGlowLabel.font =
            [UIFont systemFontOfSize:20.0
                              weight:UIFontWeightMedium];

        titleGlowLabel.adjustsFontSizeToFitWidth = YES;
        titleGlowLabel.minimumScaleFactor = 0.5;
        titleGlowLabel.numberOfLines = 1;
        titleGlowLabel.baselineAdjustment =
            UIBaselineAdjustmentAlignCenters;

        titleGlowLabel.lineBreakMode =
            NSLineBreakByClipping;

        JBTConfigureBloomLabel(
            titleGlowLabel,
            color
        );

        //
        // Sharp title foreground.
        //
        UILabel *titleLabel =
            [[UILabel alloc] init];

        titleLabel.text = titles[i];
        titleLabel.textColor = color;

        titleLabel.font =
            [UIFont systemFontOfSize:20.0
                              weight:UIFontWeightMedium];

        titleLabel.backgroundColor =
            UIColor.clearColor;

        titleLabel.adjustsFontSizeToFitWidth = YES;
        titleLabel.minimumScaleFactor = 0.5;
        titleLabel.numberOfLines = 1;
        titleLabel.baselineAdjustment =
            UIBaselineAdjustmentAlignCenters;

        titleLabel.lineBreakMode =
            NSLineBreakByClipping;

        JBTConfigureCoreLabelGlow(
            titleLabel,
            color
        );

        //
        // Value bloom.
        //
        UILabel *valueGlowLabel =
            [[UILabel alloc] init];

        valueGlowLabel.text = @"0";

        valueGlowLabel.font =
            [UIFont systemFontOfSize:20.0
                              weight:UIFontWeightRegular];

        valueGlowLabel.textAlignment =
            NSTextAlignmentRight;

        valueGlowLabel.adjustsFontSizeToFitWidth = YES;
        valueGlowLabel.minimumScaleFactor = 0.5;
        valueGlowLabel.numberOfLines = 1;
        valueGlowLabel.baselineAdjustment =
            UIBaselineAdjustmentAlignCenters;

        JBTConfigureBloomLabel(
            valueGlowLabel,
            color
        );

        //
        // Sharp value foreground.
        //
        UILabel *valueLabel =
            [[UILabel alloc] init];

        valueLabel.text = @"0";
        valueLabel.textColor = color;

        valueLabel.font =
            [UIFont systemFontOfSize:20.0
                              weight:UIFontWeightRegular];

        valueLabel.textAlignment =
            NSTextAlignmentRight;

        valueLabel.backgroundColor =
            UIColor.clearColor;

        valueLabel.adjustsFontSizeToFitWidth = YES;
        valueLabel.minimumScaleFactor = 0.5;
        valueLabel.numberOfLines = 1;
        valueLabel.baselineAdjustment =
            UIBaselineAdjustmentAlignCenters;

        JBTConfigureCoreLabelGlow(
            valueLabel,
            color
        );

        //
        // Separator.
        //
        UIView *line =
            [[UIView alloc] init];

        line.backgroundColor = color;

        JBTConfigureLineGlow(
            line,
            color
        );

        //
        // Bloom copies must be below their sharp foreground labels.
        //
        [self addSubview:titleGlowLabel];
        [self addSubview:valueGlowLabel];

        [self addSubview:titleLabel];
        [self addSubview:valueLabel];

        [self addSubview:line];

        [titleGlowLabels addObject:titleGlowLabel];
        [valueGlowLabels addObject:valueGlowLabel];

        [titleLabels addObject:titleLabel];
        [valueLabels addObject:valueLabel];

        [lines addObject:line];
    }

    self.titleGlowLabels = titleGlowLabels;
    self.valueGlowLabels = valueGlowLabels;

    self.titleLabels = titleLabels;
    self.valueLabels = valueLabels;

    self.lines = lines;

    //
    // Make PERFECT line colorful.
    //
    {
        UIColor *perfectGlowColor =
            [UIColor colorWithRed:0.45
                            green:0.95
                             blue:1.00
                            alpha:1.0];

        //
        // The visible core remains rainbow, while its bloom uses a
        // blue-white phosphor color.
        //
        self.titleGlowLabels[0].textColor =
            [perfectGlowColor colorWithAlphaComponent:0.55];

        self.valueGlowLabels[0].textColor =
            [perfectGlowColor colorWithAlphaComponent:0.55];

        self.titleGlowLabels[0].layer.shadowColor =
            perfectGlowColor.CGColor;

        self.valueGlowLabels[0].layer.shadowColor =
            perfectGlowColor.CGColor;

        self.titleLabels[0].layer.shadowColor =
            perfectGlowColor.CGColor;

        self.valueLabels[0].layer.shadowColor =
            perfectGlowColor.CGColor;

        //
        // The normal UIView background is disabled because PERFECT uses
        // a gradient layer instead.
        //
        self.lines[0].backgroundColor =
            UIColor.clearColor;

        self.perfectLineGradient =
            [CAGradientLayer layer];

        self.perfectLineGradient.colors = @[
            (id)[UIColor colorWithRed:1.00 green:0.15 blue:0.65 alpha:1.0].CGColor,
            (id)[UIColor colorWithRed:1.00 green:0.38 blue:0.35 alpha:1.0].CGColor,
            (id)[UIColor colorWithRed:1.00 green:0.70 blue:0.05 alpha:1.0].CGColor,
            (id)[UIColor colorWithRed:1.00 green:0.95 blue:0.10 alpha:1.0].CGColor,
            (id)[UIColor colorWithRed:0.55 green:0.92 blue:0.15 alpha:1.0].CGColor,
            (id)[UIColor colorWithRed:0.10 green:0.78 blue:0.55 alpha:1.0].CGColor
        ];

        self.perfectLineGradient.locations = @[
            @0.00,
            @0.20,
            @0.38,
            @0.58,
            @0.78,
            @1.00
        ];

        self.perfectLineGradient.startPoint =
            CGPointMake(0.0, 0.5);

        self.perfectLineGradient.endPoint =
            CGPointMake(1.0, 0.5);

        self.perfectLineGradient.shadowColor =
            perfectGlowColor.CGColor;

        self.perfectLineGradient.shadowOffset =
            CGSizeZero;

        self.perfectLineGradient.shadowOpacity =
            0.90;

        self.perfectLineGradient.shadowRadius =
            3.0;

        [self.lines[0].layer
            addSublayer:self.perfectLineGradient];
    }

    return self;
}

- (void)layoutSubviews
{
    [super layoutSubviews];

    self.blurView.frame =
        self.bounds;

    const CGFloat width =
        CGRectGetWidth(self.bounds);

    const CGFloat height =
        CGRectGetHeight(self.bounds);

    const CGFloat rowHeight =
        height / ((float)[self.lines count]);

    // Scale everything relative to the row height.
    const CGFloat fontSize =
        rowHeight * 0.62;

    const CGFloat horizontalPadding =
        MAX(2.0, width * 0.025);

    const CGFloat lineHeight =
        MAX(1.0, rowHeight * 0.045);

    // Values only need a relatively small column.
    const CGFloat valueWidth =
        width * 0.25;

    const CGFloat gap =
        width * 0.02;

    //
    // Keep the glow characteristics proportional to the rendered UI.
    //
    const CGFloat coreGlowRadius =
        MAX(0.75, fontSize * 0.055);

    const CGFloat bloomRadius =
        MAX(2.5, fontSize * 0.24);

    const CGFloat lineGlowRadius =
        MAX(2.0, lineHeight * 2.5);

    for (NSUInteger i = 0; i < [self.lines count]; ++i) {
        const CGFloat y =
            rowHeight * i;

        UIFont *titleFont =
            JBTTitleFont(fontSize);

        UIFont *valueFont =
            JBTValueFont(fontSize);

        //
        // Both copies must use exactly the same font. Otherwise their glyph
        // shapes diverge and the bloom becomes visibly offset.
        //
        self.titleLabels[i].font =
            titleFont;

        self.titleGlowLabels[i].font =
            titleFont;

        self.valueLabels[i].font =
            valueFont;

        self.valueGlowLabels[i].font =
            valueFont;

        self.titleLabels[i].layer.shadowRadius =
            coreGlowRadius;

        self.valueLabels[i].layer.shadowRadius =
            coreGlowRadius;

        self.titleGlowLabels[i].layer.shadowRadius =
            bloomRadius;

        self.valueGlowLabels[i].layer.shadowRadius =
            bloomRadius;

        self.lines[i].layer.shadowRadius =
            lineGlowRadius;

        const CGFloat titleWidth =
            width
            - horizontalPadding * 2.0
            - valueWidth
            - gap;

        CGRect titleFrame =
            CGRectMake(
                horizontalPadding,
                y,
                titleWidth,
                rowHeight - lineHeight
            );

        CGRect valueFrame =
            CGRectMake(
                horizontalPadding + titleWidth + gap,
                y,
                valueWidth,
                rowHeight - lineHeight
            );

        //
        // The bloom and sharp copies must occupy exactly the same rect.
        //
        self.titleGlowLabels[i].frame =
            titleFrame;

        self.titleLabels[i].frame =
            titleFrame;

        self.valueGlowLabels[i].frame =
            valueFrame;

        self.valueLabels[i].frame =
            valueFrame;

        self.lines[i].frame =
            CGRectMake(
                horizontalPadding,
                y + rowHeight - lineHeight,
                width - horizontalPadding * 2.0,
                lineHeight
            );
    }

    //
    // Give PERFECT its rainbow core.
    //
    self.titleLabels[0].textColor =
        JBTRainbowColorForSize(
            self.titleLabels[0].bounds.size
        );

    self.valueLabels[0].textColor =
        JBTRainbowColorForSize(
            self.valueLabels[0].bounds.size
        );

    self.perfectLineGradient.frame =
        self.lines[0].bounds;
}

- (void)setPerfect:(NSInteger)value
{
    _perfect = value;

    NSString *text =
        [NSString stringWithFormat:@"%ld", (long)value];

    self.valueLabels[0].text =
        text;

    self.valueGlowLabels[0].text =
        text;
}

- (void)setGreat:(NSInteger)value
{
    _great = value;

    NSString *text =
        [NSString stringWithFormat:@"%ld", (long)value];

    self.valueLabels[1].text =
        text;

    self.valueGlowLabels[1].text =
        text;
}

- (void)setGood:(NSInteger)value
{
    _good = value;

    NSString *text =
        [NSString stringWithFormat:@"%ld", (long)value];

    self.valueLabels[2].text =
        text;

    self.valueGlowLabels[2].text =
        text;
}

- (void)setPoor:(NSInteger)value
{
    _poor = value;

    NSString *text =
        [NSString stringWithFormat:@"%ld", (long)value];

    self.valueLabels[3].text =
        text;

    self.valueGlowLabels[3].text =
        text;
}

- (void)setMiss:(NSInteger)value
{
    _miss = value;

    NSString *text =
        [NSString stringWithFormat:@"%ld", (long)value];

    self.valueLabels[4].text =
        text;

    self.valueGlowLabels[4].text =
        text;
}
- (void)setSongAverageMS:(double)value
{
    _songAverageMS = value;

    NSString *text =
        [NSString stringWithFormat:@"%f ms",value];

    self.valueLabels[5].text =
        text;
    self.valueGlowLabels[5].text =
        text;
}
- (void)setRegionAverageMS:(double)value
{
    _regionAverageMS = value;

    NSString *text =
        [NSString stringWithFormat:@"%f ms",value];

    self.valueLabels[6].text =
        text;

    self.valueGlowLabels[6].text =
        text;
}
@end