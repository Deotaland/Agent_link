#include "paper4_state.h"
#include "paper4_touch.h"
#include "paper_resources_host.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
// INSERT_TOUCH_CONFIG
// INSERT_FORMAT
// INSERT_PAGE_WORK
// INSERT_TOUCH_EVENT
enum agent_link_phase_t { AGENT_LINK_PHASE_CONNECTED, AGENT_LINK_PHASE_SETUP, AGENT_LINK_PHASE_PAIRING, AGENT_LINK_PHASE_BLOCKED };
enum { PAPER_TOUCH_GT911, PAPER_TOUCH_FT6X36 };
class Router {
public:
    std::mutex mutex_;
    struct { agent_link_phase_t phase = AGENT_LINK_PHASE_CONNECTED; } status_;
    bool ringing_ = false, busy_ = false;
    Page page_ = Page::Compose, calibration_return_ = Page::Home;
    uint32_t generation_ = 1, view_ = 1;
    paper4::Ime ime_;
    paper4::TouchCalibration calibration_;
    paper4::Point calibration_raw_[4]{};
    struct { int type = PAPER_TOUCH_GT911, width = 320, height = 480; } touch_info_;
    bool touch_ready_ = true;
    int touch_width_ = 0, touch_height_ = 0;
    paper4::Confirm confirm_;
    std::atomic<int> vibration_{-1};
    int calibration_step_ = 0, sent = 0, saves = 0, reader_page_ = 0;
    size_t cursor_ = 0;
    std::string notice_, reader_, transmitted;
    void StopAudio() { ringing_ = false; }
    void EndCall() {}
    void Back() { Open(Page::Chat); }
    void Save() { ++saves; }
    void Open(Page page) { page_ = page; ++generation_; ++view_; notice_.clear(); }
    void Publish() { ++view_; }
    std::vector<std::string> Rows() { return {"first","second"}; }
    std::vector<std::string> ReaderLines() { return {reader_}; }
    void Start(Work work, const std::string& text, const std::string&, bool editor) {
        assert(work == Work::Send && editor); ++sent; transmitted = text; busy_ = true;
    }
    // INSERT_CONFIGURE_MAPPING
    // INSERT_BEGIN_COMPOSE
    // INSERT_REVIEW
    // INSERT_HANDLE_TOUCH
};
static void Type(paper4::Ime& ime, const char* text) {
    while (*text) assert(ime.Input(std::string(1,*text++)));
}
static size_t Find(const paper4::Ime& ime, const char* text) {
    for (size_t i = 0; i < ime.Candidates().size(); ++i)
        if (!std::strcmp(ime.Candidates()[i].entry.text,text)) return i;
    assert(!"Expected candidate missing"); return 0;
}
static TouchEvent Event(Router& router, paper4::TouchCommand command, const char* value = "") {
    TouchEvent result;
    result.command = command; result.page = int(router.page_); result.epoch = router.generation_;
    result.view = router.view_; result.revision = router.ime_.Revision(); result.mode = router.ime_.Mode();
    std::snprintf(result.value,sizeof result.value,"%s",value); return result;
}
static void CalibrationSample(Router& router, paper4::Point raw) {
    auto event = Event(router,paper4::TouchCommand::Calibrate);
    event.argument = router.calibration_step_;
    event.raw_x = raw.x; event.raw_y = raw.y;
    router.HandleTouch(event);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    LoadPaperResources(argv[1]);
    using namespace paper4;
    Ime ime;
    ime.SetMode(static_cast<InputMode>(99)); assert(ime.Mode() == InputMode::Nine);
    const auto started = std::chrono::steady_clock::now();
    Type(ime,"64426"); assert(ime.Select(Find(ime,"你好")) && ime.Text() == "你好");
    ime.Clear(); ime.SetMode(InputMode::Full);
    Type(ime,"nihao"); assert(ime.Select(Find(ime,"你")) && ime.Pending() == "hao");
    assert(ime.Select(Find(ime,"好")) && ime.Text() == "你好");
    ime.Backspace(); assert(ime.Text() == "你");
    ime.Clear(); Type(ime,"nv"); assert(ime.Select(Find(ime,"女")) && ime.Text() == "女");
    ime.Clear(); Type(ime,"hang"); (void)Find(ime,"行");
    ime.Clear(); Type(ime,"xing"); (void)Find(ime,"行");
    ime.Clear(); Type(ime,"ni"); ime.SetMode(InputMode::Nine); Type(ime,"426");
    ime.SetMode(InputMode::Full); assert(ime.Pending() == "ni");
    ime.SetMode(InputMode::Nine); assert(ime.Pending() == "426");
    ime.Page(-1); assert(ime.CandidatePage() < (ime.Candidates().size()+3)/4);
    assert(!ime.Select(ime.Candidates().size()));
    ime.Clear(); ime.SetMode(InputMode::English); Type(ime,"hello"); assert(ime.Space()); Type(ime,"agent");
    ime.SetMode(InputMode::Symbols); assert(ime.Input("。"));
    ime.Backspace(); assert(ime.Text() == "hello agent");
    ime.Clear(); ime.SetMode(InputMode::Full); Type(ime,"abc"); assert(ime.Literal() && ime.Text() == "abc");
    ime.Clear(); ime.SetMode(InputMode::English);
    for (size_t i = 0; i < Ime::kMaxBytes; ++i) assert(ime.Input("x"));
    assert(!ime.Input("x") && ime.Text().size() == Ime::kMaxBytes);
    std::set<std::string> characters;
    const auto& dictionary = paper_resources::Dictionary();
    for (size_t i = 0; i < dictionary.dictionary_count; ++i) {
        const auto entry = dictionary.Entry(i);
        if (std::strlen(entry.text) == 3) characters.insert(entry.text);
        assert(std::strspn(entry.pinyin,"abcdefghijklmnopqrstuvwxyz") == std::strlen(entry.pinyin));
    }
    assert(characters.size() == 6763);
    for (auto mode : {InputMode::Nine,InputMode::Full,InputMode::English,InputMode::Symbols}) {
        InputView input; input.kind = InputKind::Compose; input.mode = mode;
        input.candidates = {"你","好","你好","智能体"}; input.pinyins = {"ni","hao","nihao","zhinengti"};
        const auto keys = InputButtons(input);
        int letters = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto& a = keys[i];
            assert(a.x >= 0 && a.y >= 0 && a.x+a.w <= 528 && a.y+a.h <= 792);
            if (a.command == TouchCommand::Character && a.value.size() == 1 && a.value[0] >= 'a' && a.value[0] <= 'z') ++letters;
            for (size_t j = i+1; j < keys.size(); ++j) {
                const auto& b = keys[j];
                assert(a.x+a.w <= b.x || b.x+b.w <= a.x || a.y+a.h <= b.y || b.y+b.h <= a.y);
            }
        }
        if (mode == InputMode::Full) assert(letters == 26);
    }
    TouchCalibration calibration;
    Point raw[4];
    for (int i = 0; i < 4; ++i) raw[i] = {kCalibrationTargets[i].y*3+150,(527-kCalibrationTargets[i].x)*2+30};
    assert(calibration.Fit(raw));
    Point mapped;
    assert(calibration.Map({600*3+150,(527-100)*2+30},mapped));
    assert(std::abs(mapped.x-100)<=1 && std::abs(mapped.y-600)<=1);
    const Point invalid[] = {{1,1},{1,1},{1,1},{1,1}};
    CalibrationReport report;
    assert(!calibration.Fit(invalid,320,480,&report) && !calibration.Valid());
    assert(report.error == CalibrationError::Duplicate && report.point == 1);
    const Point collinear[] = {{20,30},{60,90},{100,150},{140,210}};
    assert(!calibration.Fit(collinear,320,480,&report) && report.error == CalibrationError::Degenerate);
    const Point inconsistent[] = {{295,340},{24,340},{24,67},{150,160}};
    assert(!calibration.Fit(inconsistent,320,480,&report) && report.error == CalibrationError::Residual);
    const auto defaults = TouchCalibration::FromRange(320,480,true,true);
    assert(defaults.Matches(320,480) && !defaults.Matches(480,320));
    const Point sensor[] = {{0,0},{319,0},{319,479},{0,479},{55,43},{45,440},{273,40}};
    const Point screen[] = {{527,791},{0,791},{0,0},{527,0},{436,720},{453,64},{76,725}};
    for (size_t i = 0; i < std::size(sensor); ++i) {
        assert(defaults.Map(sensor[i],mapped));
        assert(mapped.x == screen[i].x && mapped.y == screen[i].y);
    }
    assert(!TouchCalibration::FromRange(0,480,true,true).Valid());
    assert(!TouchCalibration::FromRange(320,9000,true,true).Valid());
    auto legacy = defaults; legacy.version = 1; assert(!legacy.Valid());
    Router mapping;
    mapping.calibration_ = legacy;
    mapping.ConfigureTouchMapping(); mapping.BeginCompose();
    assert(mapping.page_ == Page::Compose && mapping.calibration_.Matches(320,480));
    assert(mapping.calibration_.Map(sensor[4],mapped) && mapped.x == 436 && mapped.y == 720);
    mapping.touch_info_.width = 0; mapping.touch_info_.height = 0;
    mapping.ConfigureTouchMapping(); assert(mapping.calibration_.Matches(320,480));
    const Point samples[] = {{295,340},{24,340},{24,67},{295,67}};
    assert(calibration.Fit(samples,320,480,&report));
    mapping.touch_info_.width = 320; mapping.touch_info_.height = 480;
    mapping.calibration_ = calibration; mapping.ConfigureTouchMapping();
    assert(!std::memcmp(mapping.calibration_.coefficient,calibration.coefficient,sizeof calibration.coefficient));
    mapping.touch_info_.width = 480; mapping.touch_info_.height = 320;
    mapping.ConfigureTouchMapping(); assert(mapping.calibration_.Matches(480,320));
    assert(mapping.calibration_.Map({479,319},mapped) && mapped.x == 0 && mapped.y == 0);
    Router optional;
    optional.ConfigureTouchMapping(); optional.calibration_return_ = Page::Settings;
    optional.Open(Page::Calibrate);
    CalibrationSample(optional,samples[0]);
    auto stale_sample = Event(optional,TouchCommand::Calibrate); stale_sample.argument = 0;
    optional.HandleTouch(stale_sample); assert(optional.calibration_step_ == 1);
    CalibrationSample(optional,{samples[0].x+2,samples[0].y-2});
    assert(optional.calibration_step_ == 1 && !optional.notice_.empty() && optional.saves == 0);
    CalibrationSample(optional,samples[1]); assert(optional.notice_.empty());
    CalibrationSample(optional,samples[2]); CalibrationSample(optional,samples[3]);
    assert(optional.page_ == Page::Settings && optional.saves == 1 && optional.calibration_.Matches(320,480));
    const auto previous = optional.calibration_;
    optional.Open(Page::Calibrate); optional.calibration_step_ = 0;
    for (auto sample : collinear) CalibrationSample(optional,sample);
    assert(optional.page_ == Page::Settings && optional.saves == 1 && !optional.notice_.empty());
    assert(!std::memcmp(optional.calibration_.coefficient,previous.coefficient,sizeof previous.coefficient));
    assert(optional.calibration_.Map(samples[0],mapped) && mapped.x == 40 && mapped.y == 230);
    Router router;
    router.ime_.SetMode(InputMode::English);
    router.HandleTouch(Event(router,TouchCommand::Character,"h"));
    assert(router.ime_.Text() == "h" && router.sent == 0);
    const auto stale = Event(router,TouchCommand::Send);
    router.HandleTouch(Event(router,TouchCommand::Review));
    assert(router.page_ == Page::Review && router.sent == 0);
    router.HandleTouch(stale); assert(router.sent == 0);
    router.HandleTouch(Event(router,TouchCommand::Send));
    assert(router.sent == 1 && router.transmitted == "h");
    router.HandleTouch(Event(router,TouchCommand::Send)); assert(router.sent == 1);
    router.Open(Page::Compose); router.busy_ = false; router.ime_.Clear(); router.ime_.SetMode(InputMode::Full);
    Type(router.ime_,"ni"); auto old_candidate = Event(router,TouchCommand::Candidate);
    router.ime_.Input("h"); router.HandleTouch(old_candidate);
    assert(router.ime_.Text().empty());
    router.status_.phase = AGENT_LINK_PHASE_SETUP;
    router.HandleTouch(Event(router,TouchCommand::Character,"x")); assert(router.ime_.Pending() == "nih");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    std::printf("PASS: offline nine/full/English input, 6763 Hanzi, UTF-8 bounds, non-overlapping keys, calibration, stale touches and explicit-send routing (%lldms)\n",(long long)elapsed);
}
