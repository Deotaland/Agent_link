using namespace amoled_ui;
Input last_input{};
unsigned clicks = 0;
AmoledSystemMenu* volume_menu = nullptr;
bool InputTest(const Input& input, void*) {
    last_input = input;
    ++clicks;
    if (volume_menu &&
        (input.action == TouchAction::kVolume || input.action == TouchAction::kBrightness))
        volume_menu->Handle(input);
    return true;
}
void Frame(const char* filename) {
    clock_us += 200000; Render(nullptr); lv_refr_now(nullptr); CompleteTransfer(true);
    FILE* out = fopen(filename, "wb"); assert(out); fprintf(out, "P6\n360 360\n255\n");
    for (int y=0;y<360;++y) for(int x=0;x<360;++x) {
        const uint16_t v = (x-180)*(x-180)+(y-180)*(y-180) < 180*180 ? panel_->pixels[y*360+x] : 0;
        const uint8_t rgb[]{uint8_t((v>>11)*255/31), uint8_t(((v>>5)&63)*255/63), uint8_t((v&31)*255/31)};
        fwrite(rgb,1,3,out);
    }
    fclose(out);
}
void Feed(Cst9217Touch& touch, bool down, int x=0, int y=0) {
    touch.sample={down,uint16_t(x),uint16_t(y)};
    clock_us+=40000; Render(nullptr);
}
void Tap(Cst9217Touch& touch, int x, int y) {
    Feed(touch,true,x,y); Feed(touch,false);
}
void SwipeDown(Cst9217Touch& touch, int x, int y) {
    const unsigned before=clicks;
    Feed(touch,true,x,y); Feed(touch,true,x,y+70); Feed(touch,false);
    assert(clicks==before+1 && last_input.action==TouchAction::kSystem);
}
int main(int argc, char** argv) {
    assert(kDefaultLanguage == Language::kEnglish && language.load() == Language::kEnglish);
    assert(SavedLanguage(0) == Language::kChinese && SavedLanguage(1) == Language::kEnglish);
    assert(SavedLanguage(-1) == Language::kEnglish);
    // CST816 register 02 frames: explicit hardware corner vectors, including
    // event bits in the coordinate MSBs and malformed/out-of-range samples.
    const uint8_t frames[][5]{{1,0x80,0,0,0}, {1,1,0x67,0,0},
                              {1,0,0,1,0x67}, {1,1,0x67,1,0x67}};
    const int corners[][2]{{0,359}, {0,0}, {359,359}, {359,0}};
    cst9217::Point point;
    for (unsigned i=0;i<4;++i) {
        assert(sdgoods::DecodeTouch(frames[i],5,&point));
        assert(point.pressed && point.x==corners[i][0] && point.y==corners[i][1]);
    }
    const uint8_t outside[]{1,1,0x68,0,0}, multiple[]{2,0,0,0,0}, release[]{0,0,0,0,0};
    assert(!sdgoods::DecodeTouch(outside,5,&point) && !point.pressed);
    assert(!sdgoods::DecodeTouch(multiple,5,&point) && !point.pressed);
    assert(!sdgoods::DecodeTouch(frames[0],4,&point));
    assert(sdgoods::DecodeTouch(release,5,&point) && !point.pressed);
    assert(argc == 3);
    std::ifstream in(argv[1], std::ios::binary);
    assert(in);
    font_image.assign(std::istreambuf_iterator<char>(in),{});
    lv_init(); lv_tick_set_cb(Tick);
    static Co5300Panel panel; panel_=&panel; handler_=InputTest;
    static uint8_t buffers[2][360*360*2];
    auto* display=lv_display_create(360,360);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display,Flush);
    lv_display_set_buffers(display,buffers[0],buffers[1],sizeof buffers[0],LV_DISPLAY_RENDER_MODE_PARTIAL);
    fast_ui_=true;
    assert(display_transfer_.Start(panel_,sizeof buffers[0],0,360)==ESP_OK);
    lv_display_set_flush_wait_cb(display,WaitFlush);
    ConfigureTouch(&amoled_factory::Renderer());
    Screen state{}; state.kind=View::kFactoryHome; state.launcher_shell=true;
    state.view=1; state.navigation=1; state.card_count=5;
    state.storage_total=10*1024*1024; state.storage_used=4*1024*1024;
    state.battery_percent=72; state.wifi_connected=true;
    SetScreen(state); Frame("sdgoods-home.ppm");
    assert(fonts_.Ready() && LayoutFits());
    Cst9217Touch touch; touch_=&touch;
    pointer_=lv_indev_create(); lv_indev_set_type(pointer_,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer_,display); lv_indev_set_read_cb(pointer_,ReadTouch);
    const auto fresh_input=[&] {
        state.view++; SetScreen(state); clock_us+=40000; Render(nullptr);
    };
    for(int i=0;i<3;++i) {
        state.view++; SetScreen(state); Frame("sdgoods-home.ppm");
        const unsigned before=clicks; Tap(touch,90+i*90,220);
        assert(clicks==before+1 && last_input.action==TouchAction::kRow && last_input.row==i);
    }
    state.view++; state.wifi_connected=false; state.wifi_setup_active=true;
    snprintf(state.wifi_setup_ssid,sizeof state.wifi_setup_ssid,"Sugarat-Button-ABCDEF");
    SetScreen(state); Frame("sdgoods-setup.ppm");
    assert(!lv_obj_has_flag(amoled_factory::wifi_title,LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(amoled_factory::wifi_ap),state.wifi_setup_ssid));
    Tap(touch,180,110); assert(last_input.action==TouchAction::kRow && last_input.row==3);
    state.view++; SetScreen(state); Frame("sdgoods-setup.ppm");
    Tap(touch,90,220); assert(last_input.action==TouchAction::kRow && last_input.row==0);
    for(int slot=0;slot<4;++slot) {
        auto* bytes=new uint8_t[agent_system::kCoverBytes]{};
        auto pixels=std::make_shared<agent_system::CoverPixels>();
        pixels->blob=std::shared_ptr<const uint8_t>(bytes,[](const uint8_t* p){delete[] p;});
        auto* colors=reinterpret_cast<uint16_t*>(bytes+agent_system::kCoverHeaderBytes);
        auto* alpha=bytes+agent_system::kCoverHeaderBytes+132*132*2;
        for(int y=0;y<132;++y) for(int x=0;x<132;++x) {
            colors[y*132+x]=uint16_t((((x*31/131+slot*7)%32)<<11)|((y*63/131)<<5)|20);
            const float d=std::sqrt(float((2*x-131)*(2*x-131)+(2*y-131)*(2*y-131)))/2;
            alpha[y*132+x]=uint8_t(std::max(0.f,std::min(1.f,66-d))*255);
            if (slot == 1) {
                colors[y * 132 + x] = 0xF800;
                alpha[y * 132 + x] = 255;
            }
        }
        agent_system::supplied_grid[slot]=pixels;
    }
    agent_system::supplied_pixels=agent_system::supplied_grid[0];
    state.kind=View::kAppGrid; state.view++; state.navigation++;
    state.row_count=4; state.card_count=4; state.card_index=0;
    snprintf(state.title,sizeof state.title,"我的应用");
    const char* apps[]{"日常对话","BambuMonitor","音乐播放器","很长的中文应用名称"};
    for(int i=0;i<4;++i) {
        snprintf(state.rows[i],112,"%s",apps[i]); state.grid[i]={i,1};
    }
    SetScreen(state); Frame("sdgoods-apps.ppm");
    for(int i=0;i<4;++i) {
        auto* src=static_cast<const lv_image_dsc_t*>(lv_image_get_src(amoled_factory::grid_covers[i].picture));
        assert(src && src->header.w==64);
        for(int j=0;j<i;++j)
            assert(src!=lv_image_get_src(amoled_factory::grid_covers[j].picture));
        state.view++; SetScreen(state); Frame("sdgoods-apps.ppm");
        Tap(touch,119+(i%2)*122,132+(i/2)*112);
        assert(last_input.action==TouchAction::kRow && last_input.row==i);
    }
    state.view++; SetScreen(state); Frame("sdgoods-apps.ppm");
    Tap(touch,234,326); assert(last_input.row==5);
    state.card_index=3; state.row_count=1; state.view++; state.navigation++;
    SetScreen(state); Frame("sdgoods-apps-last.ppm");
    assert(lv_obj_has_flag(amoled_factory::next,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(amoled_factory::grid_tiles[1],LV_OBJ_FLAG_HIDDEN));
    Tap(touch,126,326); assert(last_input.row==4);
    state.row_count=0; state.card_index=0; state.card_count=1; state.view++; state.navigation++;
    SetScreen(state); Frame("sdgoods-empty.ppm");
    Tap(touch,180,249); assert(last_input.row==0);
    state.view++; SetScreen(state); Frame("sdgoods-empty.ppm");
    Tap(touch,83,55); assert(last_input.action==TouchAction::kBack);
    state.cover_slot=0; state.cover_revision=1;
    snprintf(state.title,sizeof state.title,"日常对话");
    const auto check_button=[](lv_obj_t* button) {
        lv_area_t box; lv_obj_get_coords(button,&box);
        for (int x : {box.x1,box.x2}) for (int y : {box.y1,box.y2})
            assert((x-180)*(x-180)+(y-180)*(y-180)<180*180);
    };
    for (auto* tile : amoled_factory::home_tiles) check_button(tile);
    for (auto* tile : amoled_factory::grid_tiles) check_button(tile);
    state.kind=View::kAppActions; state.view++; state.navigation++;
    snprintf(state.detail,sizeof state.detail,"2.0.0"); SetScreen(state); Frame("sdgoods-actions.ppm");
    check_button(amoled_factory::action_launch);
    check_button(amoled_factory::action_remove);
    assert(!lv_obj_has_flag(amoled_factory::action_shade, LV_OBJ_FLAG_HIDDEN));
    Tap(touch, 120, 230);
    assert(last_input.action == TouchAction::kRow && last_input.row == 0);
    state.view++; state.navigation++; SetScreen(state); Frame("sdgoods-actions.ppm");
    Tap(touch, 240, 230);
    assert(last_input.action == TouchAction::kRow && last_input.row == 1);
    state.kind = View::kCarousel;
    state.view++;
    state.navigation++;
    state.add_card = false;
    snprintf(state.app_id, sizeof state.app_id, "agent.status");
    snprintf(state.title, sizeof state.title, "Agent_link");
    state.card_count = 2;
    state.card_index = 0;
    // A downloaded square cover must be round even without an alpha mask.
    state.cover_slot = 1;
    snprintf(state.app_id, sizeof state.app_id, "square.cover");
    snprintf(state.title, sizeof state.title, "Downloaded app");
    SetScreen(state);
    Frame("sdgoods-carousel-round-cover.ppm");
    lv_area_t cover_box;
    lv_obj_get_coords(amoled_factory::carousel_cover.box, &cover_box);
    const int edge = lv_area_get_width(&cover_box);
    const int center_x2 = cover_box.x1 + cover_box.x2;
    const int center_y2 = cover_box.y1 + cover_box.y2;
    constexpr uint16_t background = ((0x08 >> 3) << 11) | ((0x0D >> 2) << 5) | (0x16 >> 3);
    for (int y = cover_box.y1; y <= cover_box.y2; ++y) {
        for (int x = cover_box.x1; x <= cover_box.x2; ++x) {
            const int dx = 2 * x - center_x2, dy = 2 * y - center_y2;
            const int distance = dx * dx + dy * dy;
            // Leave two pixels for the anti-aliased circle boundary.
            if (distance < (edge - 4) * (edge - 4))
                assert(panel_->pixels[y * 360 + x] == 0xF800);
            else if (distance > (edge + 4) * (edge + 4))
                assert(panel_->pixels[y * 360 + x] == background);
        }
    }
    state.cover_slot = 0;
    state.view++;
    state.navigation++;
    snprintf(state.app_id, sizeof state.app_id, "agent.status");
    snprintf(state.title, sizeof state.title, "Agent_link");
    std::ifstream cover_file(argv[2], std::ios::binary);
    assert(cover_file);
    std::vector<uint8_t> cover_bytes(std::istreambuf_iterator<char>(cover_file), {});
    assert(cover_bytes.size() == agent_system::kCoverBytes);
    auto* cover_copy = new uint8_t[cover_bytes.size()];
    memcpy(cover_copy, cover_bytes.data(), cover_bytes.size());
    auto brand_cover = std::make_shared<agent_system::CoverPixels>();
    brand_cover->blob =
        std::shared_ptr<const uint8_t>(cover_copy, [](const uint8_t* p) { delete[] p; });
    agent_system::supplied_grid[0] = brand_cover;
    SetScreen(state);
    Frame("sdgoods-carousel.ppm");
    Frame("sdgoods-carousel-logo.ppm");
    assert(lv_obj_has_flag(amoled_factory::action_shade, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_width(amoled_factory::carousel_cover.box) == 232);
    Tap(touch, 180, 165);
    assert(last_input.action == TouchAction::kRow && last_input.row == 0);
    state.cover_slot = -1;
    state.view++;
    state.navigation++;
    SetScreen(state);
    Frame("sdgoods-carousel-agent-link.ppm");
    assert(!lv_obj_has_flag(amoled_factory::carousel_cover.glyph, LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(amoled_factory::carousel_cover.glyph), "AGENT\nLINK"));
    state.kind = View::kAppActions;
    state.view++;
    state.navigation++;
    SetScreen(state);
    Frame("sdgoods-agent-link-actions.ppm");
    state.kind = View::kCarousel;
    state.view++;
    state.navigation++;
    SetScreen(state);
    Frame("sdgoods-carousel-agent-link.ppm");
    unsigned swipe_clicks = clicks;
    Feed(touch, true, 245, 180);
    Feed(touch, true, 100, 180);
    Feed(touch, false);
    assert(clicks == swipe_clicks + 1 && last_input.action == TouchAction::kNextCard);
    state.add_card = true;
    state.card_index = 1;
    state.view++;
    state.navigation++;
    snprintf(state.title, sizeof state.title, "%s", Tr("添加应用", "Add application"));
    SetScreen(state);
    Frame("sdgoods-carousel-add.ppm");
    assert(!lv_obj_has_flag(amoled_factory::carousel_plus[0], LV_OBJ_FLAG_HIDDEN));
    Tap(touch, 180, 165);
    assert(last_input.action == TouchAction::kRow && last_input.row == 0);
    state.view++;
    SetScreen(state);
    Frame("sdgoods-carousel-add.ppm");
    swipe_clicks = clicks;
    Feed(touch, true, 100, 180);
    Feed(touch, true, 245, 180);
    Feed(touch, false);
    assert(clicks == swipe_clicks + 1 && last_input.action == TouchAction::kPreviousCard);
    state.view++;
    SetScreen(state);
    Render(nullptr);
    swipe_clicks = clicks;
    Feed(touch, true, 5, 180);
    Feed(touch, true, 90, 180);
    assert(lv_obj_has_flag(edge_back_badge_, LV_OBJ_FLAG_HIDDEN));
    Feed(touch, false);
    assert(clicks == swipe_clicks + 1 && last_input.action == TouchAction::kPreviousCard);
    state.add_card = false;
    state.kind=View::kDownloadCode; state.view++; state.navigation++;
    SetScreen(state); Frame("sdgoods-download-code.ppm");
    // Help keeps its 36px appearance but accepts nearby taps like the Back arrow.
    const int help_points[][2]{{272,58},{246,58},{297,58},{272,32},{272,83}};
    for(const auto& point : help_points) {
        fresh_input(); const unsigned before=clicks;
        Tap(touch,point[0],point[1]);
        assert(clicks==before+1 && last_input.action==TouchAction::kRow && last_input.row==12);
    }
    fresh_input(); const unsigned help_before_jitter=clicks;
    Feed(touch,true,250,57); Feed(touch,true,266,59); Feed(touch,false);
    assert(clicks==help_before_jitter+1 && last_input.action==TouchAction::kRow && last_input.row==12);
    const int outside_help[][2]{{241,58},{302,58},{272,27},{272,89}};
    for(const auto& point : outside_help) {
        fresh_input(); const unsigned before=clicks;
        Tap(touch,point[0],point[1]); assert(clicks==before);
    }
    fresh_input(); SwipeDown(touch,294,58); // A drag from Help still opens the system menu once.
    fresh_input();
    const unsigned disabled_clicks=clicks;
    Tap(touch,258,309); assert(clicks==disabled_clicks);
    for (int i=0;i<12;++i) {
        if(i==11) snprintf(state.download_code,sizeof state.download_code,"000001");
        state.view++; state.navigation++; SetScreen(state); Frame("sdgoods-download-code.ppm");
        Tap(touch,102+(i%3)*78,186+(i/3)*41);
        const int ids[]{1,2,3,4,5,6,7,8,9,10,0,11};
        assert(last_input.action==TouchAction::kRow && last_input.row==ids[i]);
    }
    assert(!strcmp(lv_label_get_text(amoled_factory::code_labels[0]),"0"));
    // Lower and outer painted portions used to be rejected by the 161px inset.
    // Each contact must trigger exactly one key, including zero-coordinate releases.
    const int key_edges[][3]{{102,322,10},{75,309,10},{129,319,10},
                            {258,322,11},{285,309,11},{231,319,11},{180,322,0}};
    for(const auto& key : key_edges) {
        fresh_input(); const unsigned before=clicks;
        Tap(touch,key[0],key[1]);
        assert(clicks==before+1 && last_input.action==TouchAction::kRow && last_input.row==key[2]);
    }
    fresh_input(); unsigned before=clicks;
    Feed(touch,true,248,307); Feed(touch,true,264,309); Feed(touch,false);
    assert(clicks==before+1 && last_input.action==TouchAction::kRow && last_input.row==11);
    fresh_input(); before=clicks;
    Feed(touch,true,258,322); Feed(touch,true,258,297); Feed(touch,false);
    assert(clicks==before); // A deliberate drag is still not a click.
    fresh_input(); before=clicks;
    Feed(touch,true,75,309); Feed(touch,true,0,0); Feed(touch,true,75,309); Feed(touch,false);
    assert(clicks==before); // Leaving the physical circle cannot re-arm a key.
    fresh_input(); SwipeDown(touch,180,2);
    language.store(Language::kEnglish); state.view++; SetScreen(state);
    Frame("sdgoods-download-code-en.ppm");
    language.store(kDefaultLanguage);
    state.kind=View::kDownloadDetail; state.view++; state.navigation++;
    snprintf(state.title,sizeof state.title,"Sugarat 电台");
    snprintf(state.detail,sizeof state.detail,"0.1.1 · 3.23 MB\n下载码 000001");
    snprintf(state.rows[0],sizeof state.rows[0],"下载安装");
    SetScreen(state); Frame("sdgoods-download-detail.ppm");
    Tap(touch,120,290); assert(last_input.action==TouchAction::kRow && last_input.row==0);
    state.view++; SetScreen(state); Frame("sdgoods-download-detail.ppm");
    Tap(touch,240,290); assert(last_input.action==TouchAction::kRow && last_input.row==1);
    state.kind=View::kList; state.view++; state.navigation++; state.row_count=9;
    state.detail[0]=0;
    snprintf(state.title,sizeof state.title,"系统菜单");
    const char* rows[]{"网络与配网","当前应用","设备状态","应用服务","音量",
                       "语言 / Language","应用封面与同步","硬件与电源","屏幕亮度"};
    for(int i=0;i<9;++i) snprintf(state.rows[i],112,"%s",rows[i]);
    SetScreen(state); Frame("sdgoods-settings.ppm");
    lv_obj_scroll_to_y(list_,kRowHeight,LV_ANIM_OFF); Frame("sdgoods-settings-scrolled.ppm");
    lv_obj_scroll_to_y(list_,0,LV_ANIM_OFF);
    language.store(Language::kEnglish); state.view++; state.navigation++;
    snprintf(state.title,sizeof state.title,"System");
    const char* english_rows[]{"Network and WiFi setup","Current application","Device status",
        "App service","Volume","语言 / Language","App covers and sync","Hardware and power","Screen brightness"};
    for(int i=0;i<9;++i) snprintf(state.rows[i],112,"%s",english_rows[i]);
    SetScreen(state); Frame("sdgoods-settings-en.ppm");
    language.store(kDefaultLanguage);
    state.view++;
    state.navigation++;
    snprintf(state.title,sizeof state.title,"系统菜单");
    for(int i=0;i<9;++i) snprintf(state.rows[i],112,"%s",rows[i]);
    SetScreen(state); Frame("sdgoods-settings.ppm");
    const auto coords=[](lv_obj_t* obj) { lv_area_t area; lv_obj_get_coords(obj,&area); return area; };
    const auto row=coords(rows_[0]); Tap(touch,(row.x1+row.x2)/2,(row.y1+row.y2)/2);
    assert(last_input.action==TouchAction::kRow && last_input.row==0);
    // The common settings arrow gets the same generous hit margin as native arrows.
    fresh_input(); const auto back=coords(back_); before=clicks;
    Tap(touch,back.x1-8,(back.y1+back.y2)/2);
    assert(clicks==before+1 && last_input.action==TouchAction::kBack);
    fresh_input(); before=clicks;
    Feed(touch,true,back.x1+3,back.y1+10);
    Feed(touch,true,back.x1+19,back.y1+12); Feed(touch,false);
    assert(clicks==before+1 && last_input.action==TouchAction::kBack);
    fresh_input(); before=clicks;
    Tap(touch,back.x1-20,(back.y1+back.y2)/2); assert(clicks==before);
    fresh_input(); before=clicks;
    Feed(touch,true,(row.x1+row.x2)/2,(row.y1+row.y2)/2);
    Feed(touch,true,(row.x1+row.x2)/2,(row.y1+row.y2)/2+24); Feed(touch,false);
    assert(clicks==before); // List scrolling keeps the stricter movement guard.
    fresh_input(); SwipeDown(touch,180,2);
    state.kind=View::kInfo; state.busy=true; state.progress=63; state.view++; state.navigation++;
    snprintf(state.title,sizeof state.title,"下载安装应用");
    snprintf(state.info,sizeof state.info,"1326 / 2105 KB");
    SetScreen(state); Frame("sdgoods-progress.ppm");
    const unsigned busy_clicks=clicks; Tap(touch,120,290); assert(clicks==busy_clicks);
    fresh_input(); SwipeDown(touch,180,2); // Menus remain accessible while the worker runs.
    fresh_input();
    // Start on the last physical pixel, beyond the inset used for normal taps.
    touch.sample.x=180; touch.sample.y=359; touch.sample.pressed=true;
    clock_us+=40000; Render(nullptr); touch.sample.y=296; clock_us+=40000; Render(nullptr);
    assert(last_input.action==TouchAction::kHome);
    touch.sample.pressed=false; clock_us+=40000; Render(nullptr);
    state.kind=View::kFactoryHome; state.busy=false; state.view++; state.navigation++;
    state.wifi_connected=true; state.wifi_setup_active=false;
    snprintf(state.job_status,sizeof state.job_status,"后台下载中 · 点击下载查看");
    SetScreen(state); Frame("sdgoods-background.ppm");
    assert(strstr(lv_label_get_text(amoled_factory::status),"后台下载"));
    state.job_status[0]=0;
    SetScreen(state); Frame("sdgoods-home.ppm");
    state.battery_percent=-1; state.wifi_connected=false;
    state.wifi_setup_active=false;
    SetScreen(state); Frame("sdgoods-offline.ppm");
    assert(strstr(lv_label_get_text(amoled_factory::status),"--%"));
    assert(!lv_obj_has_flag(amoled_factory::wifi_title,LV_OBJ_FLAG_HIDDEN));
    assert(strstr(lv_label_get_text(amoled_factory::wifi_url), Tr("已安装", "Installed")));
    state.battery_percent=72; state.wifi_connected=true;
    SetScreen(state); Frame("sdgoods-home.ppm");
    assert(lv_obj_has_flag(amoled_factory::wifi_title,LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(amoled_factory::count_label,LV_OBJ_FLAG_HIDDEN));
    language.store(Language::kEnglish);
    state.view++; SetScreen(state); Frame("sdgoods-home-en.ppm");
    language.store(kDefaultLanguage);
    state.view++; SetScreen(state); Frame("sdgoods-home.ppm");
    touch.sample.x=270; touch.sample.y=160; touch.sample.pressed=true;
    clock_us+=40000; Render(nullptr); touch.sample.x=180; clock_us+=40000; Render(nullptr);
    assert(last_input.action==TouchAction::kNextCard);
    touch.sample.pressed=false; clock_us+=40000; Render(nullptr);
    state.view++; SetScreen(state); Frame("sdgoods-home.ppm");
    touch.sample.x=180; touch.sample.y=42; touch.sample.pressed=true;
    clock_us+=40000; Render(nullptr); touch.sample.y=100; clock_us+=40000; Render(nullptr);
    assert(last_input.action==TouchAction::kSystem);
    touch.sample.pressed=false; clock_us+=40000; Render(nullptr);
    fresh_input(); SwipeDown(touch,180,2);
    fresh_input(); SwipeDown(touch,80,50); // Top cap sides used to be outside the gesture strip.
    state.view++; state.navigation++; SetScreen(state); Frame("sdgoods-home.ppm");
    touch.sample.x=180; touch.sample.y=325; touch.sample.pressed=true;
    clock_us+=40000; Render(nullptr); touch.sample.y=260; clock_us+=40000; Render(nullptr);
    assert(last_input.action==TouchAction::kHome);
    touch.sample.pressed=false; clock_us+=40000; Render(nullptr);
    state={}; state.kind=View::kHardwareTest; state.local_mode=true;
    state.view=100; state.navigation=100;
    state.hardware_test.kind=HardwareTest::Kind::kTouch;
    snprintf(state.title,sizeof state.title,"触摸测试");
    snprintf(state.detail,sizeof state.detail,"依次轻点亮起的圆圈");
    snprintf(state.info,sizeof state.info,"等待触摸 · 0 / 5");
    SetScreen(state); Frame("sdgoods-touch-test.ppm");
    for(const auto& target : sugarat_factory::kTouchTargets) {
        state.view++; SetScreen(state); Render(nullptr);
        const unsigned before=clicks;
        touch.sample={}; touch.sample.x=target[0]; touch.sample.y=target[1]; touch.sample.pressed=true;
        clock_us+=40000; Render(nullptr);
        touch.sample={}; // Real release frames have no coordinates.
        clock_us+=40000; Render(nullptr);
        assert(clicks==before+1 && last_input.action==TouchAction::kTouchPoint &&
               last_input.x==target[0] && last_input.y==target[1]);
    }
    state.view++; SetScreen(state); Render(nullptr);
    const unsigned before_drag=clicks;
    touch.sample={true,180,180}; clock_us+=40000; Render(nullptr);
    touch.sample.x=260; clock_us+=40000; Render(nullptr);
    touch.sample={}; clock_us+=40000; Render(nullptr);
    assert(clicks==before_drag); // Dragging cannot satisfy a target or leave the test.
    state.view++; SetScreen(state); Render(nullptr);
    touch.sample={true,180,180}; clock_us+=40000; Render(nullptr);
    state.view++; SetScreen(state); Render(nullptr);
    touch.sample={}; clock_us+=40000; Render(nullptr);
    assert(clicks==before_drag); // A contact from a previous view must not pass.
    state.view++; SetScreen(state); Render(nullptr);
    Tap(touch,180,335);
    assert(last_input.action==TouchAction::kRow && last_input.row==0);
    state.view++; SetScreen(state); Render(nullptr);
    Tap(touch,88,58); assert(last_input.action==TouchAction::kBack);
    fresh_input(); before=clicks;
    Tap(touch,62,58); assert(clicks==before+1 && last_input.action==TouchAction::kBack);
    fresh_input(); SwipeDown(touch,180,2);
    state.view++; state.hardware_test.x=262; state.hardware_test.y=262;
    state.hardware_test.completed=5; state.hardware_test.passed=true;
    snprintf(state.detail,sizeof state.detail,"测试通过 · 五个位置均已命中");
    snprintf(state.info,sizeof state.info,"X 262  Y 262   |   5 / 5");
    SetScreen(state); Frame("sdgoods-touch-passed.ppm");
    assert(!lv_obj_has_flag(amoled_factory::touch_pass,LV_OBJ_FLAG_HIDDEN));
    state.hardware_test = {};
    state.hardware_test.kind = HardwareTest::Kind::kColors;
    state.view++;
    state.navigation++;
    Toast("This toast must not cover the screen test", 60000);
    constexpr uint16_t solid_colors[]{0x0000, 0xffff, 0xf800, 0x07e0, 0x001f};
    const char* color_frames[]{"sdgoods-screen-black.ppm", "sdgoods-screen-white.ppm",
                               "sdgoods-screen-red.ppm", "sdgoods-screen-green.ppm",
                               "sdgoods-screen-blue.ppm"};
    for (int color = 0; color < 5; ++color) {
        state.hardware_test.color_index = color;  // Only the test snapshot changes.
        SetScreen(state);
        Frame(color_frames[color]);
        assert(!lv_obj_has_flag(amoled_factory::color_screen, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_has_flag(amoled_factory::diagnostics, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_has_flag(toast_label_, LV_OBJ_FLAG_HIDDEN));
        for (int y = 0; y < 360; ++y)
            for (int x = 0; x < 360; ++x)
                if ((x - 180) * (x - 180) + (y - 180) * (y - 180) < 180 * 180)
                    assert(panel_->pixels[y * 360 + x] == solid_colors[color]);
    }
    for (const auto& point : {std::pair{180, 180}, std::pair{95, 65}, std::pair{180, 335},
                              std::pair{5, 180}, std::pair{355, 180}}) {
        state.view++;
        SetScreen(state);
        Render(nullptr);
        const auto taps = clicks;
        Tap(touch, point.first, point.second);
        assert(clicks == taps + 1 && last_input.action == TouchAction::kTouchPoint);
        assert(last_input.x == point.first && last_input.y == point.second);
    }
    state.view++;
    SetScreen(state);
    Render(nullptr);
    const auto color_gestures = clicks;
    Feed(touch, true, 8, 180);
    Feed(touch, true, 100, 180);
    assert(clicks == color_gestures && !lv_obj_has_flag(edge_back_badge_, LV_OBJ_FLAG_HIDDEN));
    Feed(touch, false);
    assert(clicks == color_gestures + 1 && last_input.action == TouchAction::kBack);
    state.view++;
    SetScreen(state);
    Render(nullptr);
    SwipeDown(touch, 180, 2);
    state.view++;
    SetScreen(state);
    Render(nullptr);
    const auto before_home = clicks;
    Feed(touch, true, 180, 335);
    Feed(touch, true, 180, 260);
    Feed(touch, false);
    assert(clicks == before_home + 1 && last_input.action == TouchAction::kHome);
    Toast("", 0);
    state.hardware_test={}; state.hardware_test.kind=HardwareTest::Kind::kLevel;
    state.hardware_test.valid=true; state.hardware_test.tilt_x=8; state.hardware_test.tilt_y=-6;
    snprintf(state.title,sizeof state.title,"水平仪");
    snprintf(state.detail,sizeof state.detail,"屏幕朝上放平，让小球居中");
    snprintf(state.info,sizeof state.info,"X +8.0  Y -6.0 deg");
    state.view++; SetScreen(state); Frame("sdgoods-level.ppm");
    assert(lv_obj_get_x(amoled_factory::level_ball)==102 && lv_obj_get_y(amoled_factory::level_ball)==46);
    // Changes to the diagnostic snapshot alone must reach the renderer.
    state.hardware_test.tilt_x=0; state.hardware_test.tilt_y=0;
    state.hardware_test.centered=true; state.hardware_test.passed=true;
    SetScreen(state); Frame("sdgoods-level-centered.ppm");
    assert(lv_obj_get_x(amoled_factory::level_ball)==70 && lv_obj_get_y(amoled_factory::level_ball)==70);
    snprintf(state.detail,sizeof state.detail,"测试通过 · 可继续观察水平");
    snprintf(state.info,sizeof state.info,"X +0.0  Y +0.0 deg");
    SetScreen(state); Frame("sdgoods-level-centered.ppm");
    state.hardware_test.valid=false; SetScreen(state); Frame("sdgoods-level-unavailable.ppm");
    assert(lv_obj_has_flag(amoled_factory::level_ball,LV_OBJ_FLAG_HIDDEN));
    language.store(Language::kEnglish);
    state.hardware_test.valid=true;
    snprintf(state.title,sizeof state.title,"Spirit level");
    snprintf(state.detail,sizeof state.detail,"Passed - level stays live");
    SetScreen(state); Frame("sdgoods-level-en.ppm");
    language.store(kDefaultLanguage);
    // Render the production update page in its idle, available, progress and error states.
    UpdatePreview updater;
    auto& update = agent_system::launcher::preview_update;
    update.current_code = 20000;
    snprintf(update.current, sizeof update.current, "2.0.0");
    snprintf(update.message, sizeof update.message, "检查 Sugarat Launcher 新版本");
    const auto update_frame = [&](const char* file) {
        const auto next = state.view + 1;
        state = {};
        state.view = next;
        state.navigation = next;
        state.launcher_shell = false; // Settings renders through AmoledSystemMenu.
        updater.RenderUpdate(state);
        SetScreen(state);
        Frame(file);
    };
    update_frame("sdgoods-update.ppm");
    assert(state.row_count == 1);
    update.state = agent_system::launcher::State::kAvailable;
    snprintf(update.version, sizeof update.version, "2.0.1");
    update.size = 2300000;
    snprintf(update.changelog, sizeof update.changelog, "优化主页体验，改善下载速度和错误提示。");
    update_frame("sdgoods-update-available.ppm");
    assert(state.row_count == 2);
    agent_system::launcher::preview_busy = true;
    update.state = agent_system::launcher::State::kDownloading;
    update.received = 1150000;
    update.elapsed_ms = 10000;
    snprintf(update.message, sizeof update.message, "正在下载到启动器 B，旧版本保持可用");
    update_frame("sdgoods-update-progress.ppm");
    assert(state.row_count == 1);
    agent_system::launcher::preview_busy = false;
    update.state = agent_system::launcher::State::kError;
    snprintf(update.message, sizeof update.message,
             "升级服务返回 HTTP 401：Missing required header");
    update_frame("sdgoods-update-error.ppm");
    assert(state.row_count == 1);
    update.state = agent_system::launcher::State::kReady;
    snprintf(update.message, sizeof update.message,
             "下载和校验完成，请重启试运行新版；失败会返回旧版");
    update_frame("sdgoods-update-ready.ppm");
    assert(state.row_count == 1);
    assert(!lv_obj_has_flag(back_, LV_OBJ_FLAG_HIDDEN));
    Tap(touch, lv_obj_get_x(back_) + lv_obj_get_width(back_) / 2,
        lv_obj_get_y(back_) + lv_obj_get_height(back_) / 2);
    assert(last_input.action == TouchAction::kBack);
    state.view++;
    SetScreen(state);
    clock_us += 40000;
    Render(nullptr);
    SwipeDown(touch, 180, 2);
    // Exercise the production menu controller as well as its touch targets.
    snprintf(agent_system::menu_descriptor.board, sizeof agent_system::menu_descriptor.board,
             "SDGOODS-ESP32S3");
    snprintf(agent_system::menu_descriptor.name, sizeof agent_system::menu_descriptor.name,
             "Sugarat Launcher");
    snprintf(agent_system::menu_descriptor.app_id, sizeof agent_system::menu_descriptor.app_id,
             "agent.launcher");
    test_wifi.connected = true;
    snprintf(test_wifi.ssid, sizeof test_wifi.ssid, "Test WiFi");
    snprintf(test_wifi.ip, sizeof test_wifi.ip, "192.168.1.20");
    AmoledSystemMenu menu;
    const auto menu_frame = [&](const char* filename) {
        const auto view = state.view + 1, navigation = state.navigation + 1;
        menu.Render(state, view, navigation, 70, false);
        SetScreen(state);
        Frame(filename);
    };
    const auto menu_row = [&](int row) {
        Input input{};
        input.action = TouchAction::kRow;
        input.row = row;
        menu.Handle(input);
    };
    const auto menu_back = [&] {
        Input input{};
        input.action = TouchAction::kBack;
        menu.Handle(input);
    };
    unsigned checks = 0;
    menu.SetUpdatePage({&checks,
                        [](const Input& input, void* context) {
                            if (input.action == TouchAction::kMenu)
                                ++*static_cast<unsigned*>(context);
                            return true;
                        },
                        [](Screen& screen, void*) {
                            snprintf(screen.title, sizeof screen.title, "Factory 固件更新");
                        }});
    menu.Open();
    menu_frame("sdgoods-quick-menu.ppm");
    assert(state.kind == View::kQuickMenu && state.row_count == 4);
    assert(!strcmp(lv_label_get_text(quick_header_), "70% · Quick Panel"));
    state.battery_percent = 42;
    SetScreen(state);
    Frame("sdgoods-quick-panel-battery.ppm");
    assert(!strcmp(lv_label_get_text(quick_header_), "42% · Quick Panel"));
    state.battery_percent = -1;
    SetBattery(-1, false);
    SetScreen(state);
    Frame("sdgoods-quick-panel-unknown.ppm");
    assert(!strcmp(lv_label_get_text(quick_header_), "--% · Quick Panel"));
    menu_frame("sdgoods-quick-menu.ppm");
    assert(!strcmp(state.rows[0], "Wi-Fi") && !strcmp(state.rows[1], "Sound") &&
           !strcmp(state.rows[2], "Exit") && !strcmp(state.rows[3], "Settings"));
    for (auto* button : quick_buttons_)
        check_button(button);
    Tap(touch, 119, 148);
    assert(last_input.action == TouchAction::kRow && last_input.row == 0);
    menu.Handle(last_input);
    menu_frame("sdgoods-panel-wifi.ppm");
    assert(strstr(state.info, "Test WiFi"));
    menu_back();
    menu_frame("sdgoods-quick-menu.ppm");
    assert(state.kind == View::kQuickMenu);
    Tap(touch, 241, 148);
    assert(last_input.action == TouchAction::kRow && last_input.row == 1);
    menu.Handle(last_input);
    menu_frame("sdgoods-panel-sound.ppm");
    assert(state.kind == View::kVolumeControl);
    menu_back();
    menu_frame("sdgoods-quick-menu.ppm");
    assert(state.kind == View::kQuickMenu);
    Tap(touch, 241, 264);
    assert(last_input.action == TouchAction::kRow && last_input.row == 3);
    menu.Handle(last_input);
    menu_frame("sdgoods-settings-grid.ppm");
    assert(state.kind == View::kSettings && state.row_count == 6);
    const char* setting_rows[]{"Brightness",    "Launcher update", "About",
                               "Device status", "Language",        "User guide"};
    for (int i = 0; i < 6; ++i)
        assert(!strcmp(state.rows[i], setting_rows[i]));
    assert(lv_obj_get_scroll_bottom(list_) > 0);
    for (int i = 0; i < 3; ++i)
        check_button(rows_[i]);
    const auto scroll_clicks = clicks;
    Feed(touch, true, 180, 250);
    Feed(touch, true, 180, 215);
    Feed(touch, true, 180, 170);
    Feed(touch, true, 180, 125);
    Feed(touch, false);
    assert(clicks == scroll_clicks && lv_obj_get_scroll_y(list_) > 0);
    lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
    menu_frame("sdgoods-settings-grid.ppm");
    lv_area_t update_row;
    lv_obj_get_coords(rows_[1], &update_row);
    Tap(touch, (update_row.x1 + update_row.x2) / 2, (update_row.y1 + update_row.y2) / 2);
    assert(last_input.action == TouchAction::kRow && last_input.row == 1);
    menu.Handle(last_input);
    assert(checks == 1);
    menu_frame("sdgoods-settings-update.ppm");
    menu_back();
    menu_frame("sdgoods-settings-grid.ppm");
    assert(state.kind == View::kSettings);
    menu_back();
    menu_frame("sdgoods-quick-menu.ppm");
    menu_row(1);
    menu_frame("sdgoods-settings-volume.ppm");
    assert(state.volume_control && state.kind == View::kVolumeControl);
    int applied_volume = -1;
    menu.SetVolumeHandler(
        [](int value, void* context) {
            *static_cast<int*>(context) = value;
            return true;
        },
        &applied_volume);
    volume_menu = &menu;
    const auto saves = volume_saves;
    Feed(touch, true, 180, 250);
    Feed(touch, true, 180, 200);
    Feed(touch, true, 180, 140);
    const int raised = lv_slider_get_value(vertical_slider_);
    assert(raised >= 75 && raised <= 85 && applied_volume == raised);
    assert(volume_saves == saves && last_input.action == TouchAction::kVolume);
    Feed(touch, false);
    assert(last_input.commit && last_input.row == raised);
    assert(menu_settings["agent_nvs/voice/vol"] == raised && volume_saves == saves + 1);
    menu_frame("sdgoods-volume-raised.ppm");
    Feed(touch, true, 180, 150);
    Feed(touch, true, 180, 250);
    Feed(touch, true, 180, 303);
    Feed(touch, false);
    assert(lv_slider_get_value(vertical_slider_) == 0 && last_input.action == TouchAction::kVolume);
    assert(last_input.commit && menu_settings["agent_nvs/voice/vol"] == 0);
    menu_frame("sdgoods-volume-muted.ppm");
    Feed(touch, true, 180, 250);
    Feed(touch, true, 180, 98);
    Feed(touch, false);
    assert(lv_slider_get_value(vertical_slider_) == 100 &&
           menu_settings["agent_nvs/voice/vol"] == 100);
    menu_frame("sdgoods-volume-full.ppm");
    // Horizontal finger drift in the pillar cannot be mistaken for Back.
    Feed(touch, true, 180, 200);
    Feed(touch, true, 275, 200);
    Feed(touch, false);
    assert(last_input.action == TouchAction::kVolume);
    menu_frame("sdgoods-volume.ppm");
    volume_menu = nullptr;
    const auto* saved_renderer = card_renderer_;
    const CardRenderer::TouchPolicy native_touch{true, 12, 0, true, true};
    ConfigureTouch(nullptr,
                   native_touch);  // Native Agent_link supplies touch policy without cards.
    for (const auto& point :
         {std::pair{95, 51}, std::pair{78, 68}, std::pair{112, 68}, std::pair{95, 85}}) {
        menu_frame("sdgoods-volume-native.ppm");
        const auto before_back = clicks;
        Tap(touch, point.first, point.second);
        assert(clicks == before_back + 1 && last_input.action == TouchAction::kBack);
    }
    ConfigureTouch(saved_renderer);
    menu_back();
    menu_frame("sdgoods-quick-menu.ppm");
    menu_row(3);
    menu_frame("sdgoods-settings-grid.ppm");
    menu_row(0);
    menu_frame("sdgoods-settings-brightness.ppm");
    assert(state.kind == View::kBrightnessControl &&
           lv_slider_get_min_value(vertical_slider_) == 5);
    assert(!strcmp(lv_label_get_text(vertical_title_), "Brightness"));
    assert(!lv_obj_has_flag(vertical_sun_, LV_OBJ_FLAG_HIDDEN));
    const int unchanged_volume = volume_;
    const auto brightness_writes = brightness_saves;
    volume_menu = &menu;
    Feed(touch, true, 180, 250);
    Feed(touch, true, 180, 140);
    const int bright = lv_slider_get_value(vertical_slider_);
    assert(bright >= 75 && bright <= 85 && backlight_.load() == bright);
    assert(brightness_saves == brightness_writes && volume_ == unchanged_volume);
    assert(last_input.action == TouchAction::kBrightness);
    Feed(touch, false);
    assert(last_input.commit && brightness_saves == brightness_writes + 1);
    assert(menu_settings["agent_nvs/button_hw/bright"] == bright);
    menu_frame("sdgoods-brightness.ppm");
    Feed(touch, true, 180, 200);
    Feed(touch, true, 180, 303);
    Feed(touch, false);
    assert(backlight_.load() == 5 && menu_settings["agent_nvs/button_hw/bright"] == 5);
    menu_frame("sdgoods-brightness-min.ppm");
    Feed(touch, true, 180, 250);
    Feed(touch, true, 180, 98);
    Feed(touch, false);
    assert(backlight_.load() == 100 && menu_settings["agent_nvs/button_hw/bright"] == 100);
    assert(volume_ == unchanged_volume);
    menu_frame("sdgoods-brightness-max.ppm");
    volume_menu = nullptr;
    menu_back();
    menu_frame("sdgoods-settings-grid.ppm");
    menu_row(4);
    menu_frame("sdgoods-settings-language.ppm");
    assert(state.row_count == 2 && !strcmp(state.rows[1], "English"));
    menu_back();
    menu_row(5);
    menu_frame("sdgoods-user-guide.ppm");
    assert(state.row_count == kUserGuideTopicCount && !strcmp(state.title, "User guide"));
    for (const auto selected_language : {Language::kEnglish, Language::kChinese}) {
        language.store(selected_language);
        for (int topic = 0; topic < kUserGuideTopicCount; ++topic) {
            const auto guide = SugaratUserGuide(topic);
            assert(strlen(guide.body) < sizeof state.info);
            menu_row(topic);
            menu_frame(topic == 2 ? "sdgoods-user-guide-install.ppm"
                                  : "sdgoods-user-guide-topic.ppm");
            assert(!strcmp(state.title, guide.title) && !strcmp(state.info, guide.body));
            assert(!strcmp(lv_label_get_text(list_info_), guide.body));
            if (topic == 2) {
                assert(strstr(state.info, "https://sugarat.vercel.app/") &&
                       lv_obj_get_scroll_bottom(list_) > 0);
                lv_obj_scroll_to_y(list_, lv_obj_get_scroll_bottom(list_), LV_ANIM_OFF);
                Frame("sdgoods-user-guide-install-scrolled.ppm");
            }
            menu_back();
        }
    }
    language.store(Language::kEnglish);
    menu_row(2);
    menu_frame("sdgoods-user-guide-install.ppm");
    menu_back();
    menu_back();
    menu.SetExtraPage({nullptr, nullptr, [](Screen& screen, void*) {
                           snprintf(screen.title, sizeof screen.title, "Device tools");
                       }});
    menu_frame("sdgoods-settings-grid.ppm");
    assert(state.row_count == 7 && !strcmp(state.rows[6], "Device tools"));
    menu_row(6);
    menu_frame("sdgoods-settings-hardware.ppm");
    assert(!strcmp(state.title, "Device tools"));
    menu_back();
    menu.SetExtraPage({});
    menu_frame("sdgoods-settings-grid.ppm");
    lv_obj_scroll_to_y(list_, 200, LV_ANIM_OFF);
    Frame("sdgoods-settings-grid-scrolled.ppm");
    assert(lv_obj_get_scroll_y(list_) > 0);
    menu_back();
    menu_frame("sdgoods-quick-menu.ppm");
    Tap(touch, 241, 264);
    assert(last_input.row == 3);
    menu.Handle(last_input);
    menu_row(2);  // About remains available in Settings.
    menu_frame("sdgoods-current-app.ppm");
    assert(strstr(state.info, "App: Sugarat Launcher\n") && strstr(state.info, "1.0.0") &&
           strstr(state.info, "Test WiFi") && strstr(state.info, "192.168.1.20"));
    menu_back();
    menu_frame("sdgoods-settings-grid.ppm");
    assert(state.kind == View::kSettings);
    menu_back();
    menu_frame("sdgoods-quick-menu.ppm");
    Tap(touch, 119, 264);
    assert(last_input.row == 2);
    menu.Handle(last_input);
    menu_frame("sdgoods-launcher-exit.ppm");
    assert(state.kind == View::kQuickMenu && !menu.TakeExit());
    assert(!strcmp(lv_label_get_text(toast_label_), "Already in Launcher"));
    menu_row(2);
    assert(!menu.TakeExit());
    // Only an installed application can request a confirmed Launcher return.
    agent_system::menu_launcher = false;
    snprintf(agent_system::menu_descriptor.name, sizeof agent_system::menu_descriptor.name,
             "Agent Voice");
    snprintf(agent_system::menu_descriptor.app_id, sizeof agent_system::menu_descriptor.app_id,
             "agent.status");
    Toast("", 0);  // An actual app switch starts a fresh UI instance.
    ConfigureTouch(nullptr, native_touch);
    // Reproduce the app's actual no-renderer input path at the physical top cap,
    // including setup/binding, a busy call and ordinary subpages.
    for (int scenario = 0; scenario < 7; ++scenario) {
        agent_link_status_t native_status{};
        native_status.phase = scenario == 1   ? AGENT_LINK_PHASE_SETUP
                              : scenario == 2 ? AGENT_LINK_PHASE_PAIRING
                                              : AGENT_LINK_PHASE_READY;
        SetStatus(native_status);
        for (const auto& start : {std::pair{180, 2}, std::pair{80, 50}, std::pair{280, 50}}) {
            const auto view = state.view + 1;
            state = {};
            state.view = view;
            state.navigation = view;
            state.kind = scenario < 4    ? View::kHome
                         : scenario == 4 ? View::kList
                         : scenario == 5 ? View::kVolumeControl
                                         : View::kBrightnessControl;
            state.busy = state.calling = scenario == 3;
            SetScreen(state);
            Frame("sdgoods-native-before-pulldown.ppm");
            SwipeDown(touch, start.first, start.second);
        }
    }
    state.view++;
    state.navigation++;
    state.kind = View::kList;
    state.busy = state.calling = false;
    SetScreen(state);
    Render(nullptr);
    const auto before_content_scroll = clicks;
    Feed(touch, true, 180, 150);
    Feed(touch, true, 180, 230);
    Feed(touch, false);
    assert(clicks == before_content_scroll);  // Content scrolling is not a pull-down.
    Feed(touch, true, 180, 2);
    state.view++;
    SetScreen(state);
    Render(nullptr);
    Feed(touch, true, 180, 72);
    Feed(touch, false);
    assert(clicks == before_content_scroll);  // A stale gesture cannot open a new view's menu.
    menu.Open();
    menu_frame("sdgoods-native-pulldown.ppm");
    assert(!card_renderer_ && state.kind == View::kQuickMenu && state.row_count == 4);
    assert(!strcmp(state.rows[0], "Wi-Fi") && !strcmp(state.rows[1], "Sound") &&
           !strcmp(state.rows[2], "Exit") && !strcmp(state.rows[3], "Settings"));
    const auto before_cancel = clicks;
    Feed(touch, true, 8, 180);
    Feed(touch, true, 90, 180);
    Frame("sdgoods-edge-back-preview.ppm");
    assert(clicks == before_cancel && !lv_obj_has_flag(edge_back_badge_, LV_OBJ_FLAG_HIDDEN));
    Feed(touch, true, 8, 180);
    assert(lv_obj_has_flag(edge_back_badge_, LV_OBJ_FLAG_HIDDEN));
    Feed(touch, false);
    assert(clicks == before_cancel && menu.Active());
    menu_frame("sdgoods-native-pulldown.ppm");
    Feed(touch, true, 8, 180);
    Feed(touch, true, 90, 180);
    Feed(touch, true, 0, 0);  // Leaving the physical circle cancels the whole contact.
    Feed(touch, true, 90, 180);
    assert(lv_obj_has_flag(edge_back_badge_, LV_OBJ_FLAG_HIDDEN));
    Feed(touch, false);
    assert(clicks == before_cancel && menu.Active());
    Feed(touch, true, 8, 180);
    Feed(touch, true, 90, 180);
    menu_frame("sdgoods-native-pulldown.ppm");  // A new view invalidates a held gesture.
    Feed(touch, false);
    assert(clicks == before_cancel && lv_obj_has_flag(edge_back_badge_, LV_OBJ_FLAG_HIDDEN));
    menu.Open();
    menu_row(3);
    menu_frame("sdgoods-native-settings.ppm");
    const auto before_back_commit = clicks;
    Feed(touch, true, 90, 180);
    Feed(touch, true, 190, 180);
    Feed(touch, false);
    assert(clicks == before_back_commit && lv_obj_has_flag(edge_back_badge_, LV_OBJ_FLAG_HIDDEN));
    Feed(touch, true, 8, 180);
    Feed(touch, true, 100, 180);
    clock_us += 2000000;
    Render(nullptr);  // Holding does not submit or time out Back.
    assert(clicks == before_back_commit && !lv_obj_has_flag(edge_back_badge_, LV_OBJ_FLAG_HIDDEN));
    Feed(touch, false);
    assert(clicks == before_back_commit + 1 && last_input.action == TouchAction::kBack);
    menu.Handle(last_input);
    menu_frame("sdgoods-native-pulldown.ppm");
    assert(menu.Active() && state.kind == View::kQuickMenu);  // Exactly one parent, not home.
    menu_row(3);
    menu_row(2);
    menu_frame("sdgoods-native-current-app.ppm");
    assert(strstr(state.info, "App: Agent_link\n"));
    menu_back();
    menu_frame("sdgoods-native-settings.ppm");
    assert(state.row_count == 7 && !strcmp(state.rows[2], "About") &&
           !strcmp(state.rows[4], "Language") && !strcmp(state.rows[5], "User guide") &&
           !strcmp(state.rows[6], "Return to launcher"));
    menu_row(0);
    menu_frame("sdgoods-brightness-native.ppm");
    Tap(touch, 95, 68);
    assert(last_input.action == TouchAction::kBack);
    menu.Handle(last_input);
    menu_row(0);
    menu_frame("sdgoods-brightness-native.ppm");
    Feed(touch, true, 180, 330);
    Feed(touch, true, 180, 260);
    Feed(touch, false);
    assert(last_input.action == TouchAction::kHome && volume_ == unchanged_volume);
    menu.Handle(last_input);
    assert(!menu.Active());
    menu.Open();
    menu_row(2);
    menu_frame("sdgoods-exit-confirm.ppm");
    assert(!menu.TakeExit());
    menu_row(0);
    menu_frame("sdgoods-quick-menu.ppm");
    assert(!menu.TakeExit());
    menu_row(2);
    menu_row(1);
    assert(menu.TakeExit() && !menu.TakeExit());
    menu.Open();
    menu_row(1);
    menu_frame("sdgoods-volume.ppm");
    const int before_back_swipe = volume_;
    Feed(touch, true, 8, 180);
    Feed(touch, true, 100, 180);
    Feed(touch, false);
    assert(last_input.action == TouchAction::kBack && volume_ == before_back_swipe);
    menu.Handle(last_input);
    for (int start : {310, 330, 350}) {
        menu.Open();
        menu_row(1);
        menu_frame("sdgoods-volume.ppm");
        const int before_home = volume_;
        Feed(touch, true, 180, start);
        Feed(touch, true, 180, start - 70);
        Feed(touch, false);
        assert(last_input.action == TouchAction::kHome && volume_ == before_home);
        menu.Handle(last_input);
        assert(!menu.Active());
    }
    // Native apps hand off only a navigation request; Factory alone performs OTA.
    agent_system::menu_launcher = false;
    menu.SetUpdatePage({});
    menu.Open();
    menu_row(3);
    menu_row(1);
    menu_frame("sdgoods-app-factory-info.ppm");
    menu_row(0);
    assert(!menu.TakeExit());
    menu_row(0);
    assert(!menu.TakeExit() && !AmoledSystemMenu::TakeUpdateRequest());
    menu_row(0);
    menu_row(1);
    assert(menu.TakeExit());
    assert(AmoledSystemMenu::TakeUpdateRequest() && !AmoledSystemMenu::TakeUpdateRequest());
    menu.Close();
    agent_system::menu_launcher = true;
    ConfigureTouch(saved_renderer);
    // The activation URL must accompany the code only during device binding.
    agent_link_status_t binding{};
    binding.phase = AGENT_LINK_PHASE_PAIRING;
    snprintf(binding.code, sizeof binding.code, "123456");
    snprintf(binding.hint, sizeof binding.hint, "Enter this code in the console - 120s left");
    SetStatus(binding);
    const auto binding_view = state.view + 1;
    state = {};
    state.kind = View::kHome;
    state.view = binding_view;
    state.navigation = binding_view;
    snprintf(state.info, sizeof state.info, "%s", DeviceBindingGuide());
    SetScreen(state);
    Frame("sdgoods-device-binding.ppm");
    assert(!strcmp(lv_label_get_text(list_code_), "123456"));
    assert(strstr(lv_label_get_text(list_info_), "https://bot.deotaland.cn/#/devices"));
    lv_point_t link_size{};
    lv_text_get_size(&link_size, "https://bot.deotaland.cn/#/devices", fonts_.Small(), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    assert(link_size.x <= lv_obj_get_width(list_info_));
    assert(Fits(ScaledRect(39, 181, 388, 125)));
    binding.phase = AGENT_LINK_PHASE_SETUP;
    snprintf(binding.hint, sizeof binding.hint, "Join the device hotspot to set up WiFi");
    SetStatus(binding);
    Frame("sdgoods-device-wifi-setup.ppm");
    assert(lv_obj_has_flag(list_code_, LV_OBJ_FLAG_HIDDEN));
    assert(!strstr(lv_label_get_text(list_info_), "bot.deotaland.cn"));
    binding.phase = AGENT_LINK_PHASE_READY;
    SetStatus(binding);
    Frame("sdgoods-device-ready.ppm");
    assert(lv_obj_has_flag(list_, LV_OBJ_FLAG_HIDDEN));
    state.kind = View::kDownloadHelp;
    state.launcher_shell = true;
    state.view++;
    state.navigation++;
    snprintf(state.title, sizeof state.title, "%s", Tr("下载码在哪里？", "Where is the code?"));
    snprintf(state.info, sizeof state.info, "%s", DownloadCodeGuide());
    state.row_count = 1;
    snprintf(state.rows[0], sizeof state.rows[0], "%s",
             Tr("返回输入下载码", "Enter download code"));
    SetScreen(state);
    Frame("sdgoods-download-code-help.ppm");
    assert(strstr(lv_label_get_text(list_info_), "https://sugarat.vercel.app/"));
    AmoledLauncherModel carousel;
    carousel.SetCount(2);
    assert(carousel.Move(1, true) && carousel.Index() == 1);
    assert(carousel.Move(1, true) && carousel.IsAdd());
    assert(carousel.Move(1, true) && carousel.Index() == 0);
    assert(carousel.Move(-1, true) && carousel.IsAdd());
    carousel.SetCount(0);
    assert(carousel.IsAdd() && !carousel.Move(1, true));
    SetBacklight(42);
    Frame("sdgoods-backlight.ppm");
    assert(applied_backlight_ == (42*255+50)/100);
    sleep_requested_.store(true);
    Frame("sdgoods-sleep.ppm");
    assert(sleep_ready_.load() && applied_backlight_ == 0);
    CancelSleep();
    Frame("sdgoods-resume.ppm");
    assert(!sleep_ready_.load() && applied_backlight_ == (42*255+50)/100);
    lv_indev_delete(pointer_); pointer_=nullptr; display_transfer_.Stop();
    puts("SDGOODS: keypad edges, Back margins, button jitter, drag/circle/stale guards, top-edge menu on home/download/settings/progress/tests, live level and launcher pages passed.");
    return 0;
}
