// JBTJudgeView.h

#import <UIKit/UIKit.h>
#define FPS_KEY @"KISS_FORCED_FPS"
#define INPUT_FIX_KEY @"KISS_INPUT_EDGE_FIX"
#define ANALYZER_KEY @"KISS_ENABLED_ANALYZER"
#define REALTIME_ANALYZER_KEY @"KISS_ENABLED_REALTIME_ANALYZER"
typedef NS_ENUM(NSInteger, JBTSettingsSection) {
    JBTSettingsSectionGeneral = 0,
    JBTSettingsSectionAnalyzer,
    JBTSettingsSectionCount,
};
typedef NS_ENUM(NSInteger, JBTGeneralRow) {
    JBTGeneralRowFrameRate = 0,
    JBTGeneralRowInputEdgeFix,
    JBTGeneralRowCount,
};
typedef NS_ENUM(NSInteger, JBTAnalyzerRow) {
    JBTAnalyzerRowEnabled = 0,
    JBTAnalyzerRowEnableRealtimeJudge,
    JBTAnalyzerRowCount,
};
@interface JBTSettingsViewController : UITableViewController
- (void)viewDidLoad;
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView;
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section;
- (UITableViewCell *)tableView:(UITableView *)tableView
        cellForRowAtIndexPath:(NSIndexPath *)indexPath;
- (void)setAnalyzer:(UISwitch *)sender;
- (void)setAnalyzerRealtime:(UISwitch *)sender;
- (void)setInputEdgeFix:(UISwitch *)sender;
- (void)showFPSMenu:(UIButton *)sender;
- (void)closeSettings;
@end
#ifdef __cplusplus
extern "C" {
#endif
    NSInteger selectedFPS();
    BOOL inputEdgeFixEnabled();
    BOOL analyzerEnabled();
    BOOL analyzerRealtimeEnabled();
#ifdef __cplusplus
}
#endif