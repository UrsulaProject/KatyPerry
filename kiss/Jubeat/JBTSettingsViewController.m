// JBTSettingsViewController.m

#import "Jubeat/JBTSettingsViewController.h"
#import <QuartzCore/QuartzCore.h>
#ifdef __cplusplus
extern "C" {
#endif
NSInteger selectedFPS() {
  NSInteger fps = [[NSUserDefaults standardUserDefaults] integerForKey:FPS_KEY];
  return fps ? fps : 30;
}

BOOL inputEdgeFixEnabled() {
  id value = [[NSUserDefaults standardUserDefaults] objectForKey:INPUT_FIX_KEY];
  return value ? [value boolValue] : YES;
}
BOOL analyzerEnabled() {
  id value = [[NSUserDefaults standardUserDefaults] objectForKey:ANALYZER_KEY];
  return value ? [value boolValue] : NO;
}
BOOL analyzerRealtimeEnabled() {
  id value = [[NSUserDefaults standardUserDefaults]
      objectForKey:REALTIME_ANALYZER_KEY];
  return value ? [value boolValue] : NO;
}
#ifdef __cplusplus
}
#endif

@implementation JBTSettingsViewController
- (void)viewDidLoad {
  [super viewDidLoad];
  [self setTitle:@"KISS"];
  [self navigationItem].leftBarButtonItem = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                           target:self
                           action:@selector(closeSettings)] autorelease];

  UIView *background =
      [[[UIView alloc] initWithFrame:CGRectMake(0, 0, 0, 0)] autorelease];
  UILabel *version =
      [[[UILabel alloc] initWithFrame:CGRectMake(0, 0, 0, 0)] autorelease];
  UIFontDescriptorSymbolicTraits traits =
      UIFontDescriptorTraitBold | UIFontDescriptorTraitItalic;
  UIFontDescriptor *descriptor =
      [[[UIFont systemFontOfSize:[UIFont smallSystemFontSize]] fontDescriptor]
          fontDescriptorWithSymbolicTraits:traits];
  version.font = [UIFont fontWithDescriptor:descriptor size:0];
  version.text =
      [NSString stringWithFormat:@"KISS %s @ %s", GIT_COMMIT_HASH, GIT_REFSPEC];
  version.textAlignment = NSTextAlignmentCenter;
  version.textColor = [UIColor grayColor];
  version.translatesAutoresizingMaskIntoConstraints = NO;
  [background addSubview:version];
  [NSLayoutConstraint activateConstraints:@[
    [version.leadingAnchor constraintEqualToAnchor:background.leadingAnchor],
    [version.trailingAnchor constraintEqualToAnchor:background.trailingAnchor],
    [version.bottomAnchor
        constraintEqualToAnchor:background.layoutMarginsGuide.bottomAnchor]
  ]];
  [self tableView].backgroundView = background;
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
  return JBTSettingsSectionCount;
}
- (NSInteger)tableView:(UITableView *)tableView
    numberOfRowsInSection:(NSInteger)section {
  switch (section) {
  case JBTSettingsSectionGeneral:
    return JBTGeneralRowCount;

  case JBTSettingsSectionAnalyzer:
    return analyzerEnabled() ? JBTAnalyzerRowCount : 1;

  default:
    return 0;
  }
}
- (NSString *)tableView:(UITableView *)tableView
    titleForHeaderInSection:(NSInteger)section {
  switch (section) {
  case JBTSettingsSectionGeneral:
    return @"General";

  case JBTSettingsSectionAnalyzer:
    return @"Jubeat Analyzer";

  default:
    return nil;
  }
}
- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
  UITableViewCell *cell =
      [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                              reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  switch (indexPath.section) {
  case JBTSettingsSectionGeneral:
    [self configureGeneralCell:cell row:indexPath.row];
    break;

  case JBTSettingsSectionAnalyzer:
    [self configureAnalyzerCell:cell row:indexPath.row];
    break;
  }

  return cell;
}
- (void)setAnalyzer:(UISwitch *)sender {
  [[NSUserDefaults standardUserDefaults] setBool:sender.on forKey:ANALYZER_KEY];
  NSIndexSet *sections =
      [NSIndexSet indexSetWithIndex:JBTSettingsSectionAnalyzer];

  [self.tableView reloadSections:sections
                withRowAnimation:UITableViewRowAnimationAutomatic];
}
- (void)setAnalyzerRealtime:(UISwitch *)sender {
  [[NSUserDefaults standardUserDefaults] setBool:sender.on forKey:REALTIME_ANALYZER_KEY];
}
- (void)setInputEdgeFix:(UISwitch *)sender {
  [[NSUserDefaults standardUserDefaults] setBool:sender.on
                                          forKey:INPUT_FIX_KEY];
}
- (void)showFPSMenu:(UIButton *)sender {
  UIAlertController *menu = [UIAlertController
      alertControllerWithTitle:nil
                       message:nil
                preferredStyle:UIAlertControllerStyleActionSheet];
  NSInteger selected = selectedFPS();
  NSArray *rates = [UIScreen mainScreen].maximumFramesPerSecond >= 120
                       ? @[ @30, @60, @120 ]
                       : @[ @30, @60 ];
  for (NSNumber *rate in rates) {
    NSString *title = [NSString
        stringWithFormat:@"%@%@ FPS",
                         selected == rate.integerValue ? @"✓ " : @"", rate];
    [menu
        addAction:[UIAlertAction
                      actionWithTitle:title
                                style:UIAlertActionStyleDefault
                              handler:^(UIAlertAction *action) {
                                [[NSUserDefaults standardUserDefaults]
                                    setInteger:rate.integerValue
                                        forKey:FPS_KEY];
                                [sender
                                    setTitle:[NSString
                                                 stringWithFormat:@"%@ FPS  ▾",
                                                                  rate]
                                    forState:UIControlStateNormal];
                                [sender sizeToFit];
                              }]];
  }
  [menu addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                           style:UIAlertActionStyleCancel
                                         handler:nil]];
  menu.popoverPresentationController.sourceView = sender;
  menu.popoverPresentationController.sourceRect = sender.bounds;
  [self presentViewController:menu animated:YES completion:nil];
}
- (void)configureGeneralCell:(UITableViewCell *)cell row:(NSInteger)row {
  switch (row) {
  case JBTGeneralRowFrameRate: {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    NSInteger selected = selectedFPS();

    [button addTarget:self
                  action:@selector(showFPSMenu:)
        forControlEvents:UIControlEventTouchUpInside];
    [button setTitle:[NSString stringWithFormat:@"%ld FPS  ▾", (long)selected]
            forState:UIControlStateNormal];
    [button sizeToFit];

    cell.textLabel.text = @"Frame Rate";
    cell.accessoryView = button;
    break;
  }

  case JBTGeneralRowInputEdgeFix: {
    UISwitch *toggle = [[[UISwitch alloc] init] autorelease];

    toggle.on = inputEdgeFixEnabled();

    [toggle addTarget:self
                  action:@selector(setInputEdgeFix:)
        forControlEvents:UIControlEventValueChanged];

    cell.textLabel.text = @"Input Fix";
    cell.accessoryView = toggle;
    break;
  }
  }
}

- (void)configureAnalyzerCell:(UITableViewCell *)cell row:(NSInteger)row {
  switch (row) {
  case JBTAnalyzerRowEnabled: {
    UISwitch *toggle = [[[UISwitch alloc] init] autorelease];

    toggle.on = analyzerEnabled();

    [toggle addTarget:self
                  action:@selector(setAnalyzer:)
        forControlEvents:UIControlEventValueChanged];

    cell.textLabel.text = @"Enable";
    cell.accessoryView = toggle;
    break;
  }
  case JBTAnalyzerRowEnableRealtimeJudge: {
    UISwitch *toggle = [[[UISwitch alloc] init] autorelease];

    toggle.on = analyzerRealtimeEnabled();

    [toggle addTarget:self
                  action:@selector(setAnalyzerRealtime:)
        forControlEvents:UIControlEventValueChanged];

    cell.textLabel.text = @"Enable RealTime (Requires Powerful Device to not lag)";
    cell.accessoryView = toggle;
    break;
  }
  }
}
- (NSString *)tableView:(UITableView *)tableView
    titleForFooterInSection:(NSInteger)section {
  switch (section) {
  case JBTSettingsSectionAnalyzer:
    return @"Changes to JubeatAnalyzer take effect after restarting the game.";

  default:
    return nil;
  }
}
- (void)closeSettings {
  [[NSUserDefaults standardUserDefaults] synchronize];
  [self dismissViewControllerAnimated:YES completion:nil];
}
@end