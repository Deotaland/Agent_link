using namespace amoled_ui;
Input last_input{};
unsigned clicks = 0;
bool InputTest(const Input& input, void*) { last_input = input; ++clicks; return true; }
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
void Tap(Cst9217Touch& touch, int x, int y) {
    touch.sample.x=x; touch.sample.y=y; touch.sample.pressed=true;
    clock_us+=40000; Render(nullptr);
    touch.sample.pressed=false; clock_us+=40000; Render(nullptr);
}
int main(int argc, char** argv) {
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
    assert(argc==2); std::ifstream in(argv[1],std::ios::binary); assert(in);
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
    card_renderer_=&amoled_factory::Renderer();
    Screen state{}; state.kind=View::kFactoryHome; state.launcher_shell=true;
    state.view=1; state.navigation=1; state.card_count=5;
    state.storage_total=10*1024*1024; state.storage_used=4*1024*1024;
    state.battery_percent=72; state.wifi_connected=true;
    SetScreen(state); Frame("sdgoods-home.ppm");
    assert(fonts_.Ready() && LayoutFits());
    Cst9217Touch touch; touch_=&touch;
    pointer_=lv_indev_create(); lv_indev_set_type(pointer_,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer_,display); lv_indev_set_read_cb(pointer_,ReadTouch);
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
    check_button(amoled_factory::launch); check_button(amoled_factory::remove);
    Tap(touch,120,290); assert(last_input.action==TouchAction::kRow && last_input.row==0);
    state.view++; state.navigation++; SetScreen(state); Frame("sdgoods-actions.ppm");
    Tap(touch,240,290); assert(last_input.action==TouchAction::kRow && last_input.row==1);
    state.kind=View::kDownloadCode; state.view++; state.navigation++;
    SetScreen(state); Frame("sdgoods-download-code.ppm");
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
    language.store(Language::kEnglish); state.view++; SetScreen(state);
    Frame("sdgoods-download-code-en.ppm"); language.store(Language::kChinese);
    state.kind=View::kDownloadDetail; state.view++; state.navigation++;
    snprintf(state.title,sizeof state.title,"Sugarat 电台");
    snprintf(state.detail,sizeof state.detail,"0.1.1 · 3.23 MB\n下载码 000001");
    snprintf(state.rows[0],sizeof state.rows[0],"下载安装");
    SetScreen(state); Frame("sdgoods-download-detail.ppm");
    Tap(touch,120,290); assert(last_input.action==TouchAction::kRow && last_input.row==0);
    state.view++; SetScreen(state); Frame("sdgoods-download-detail.ppm");
    Tap(touch,240,290); assert(last_input.action==TouchAction::kRow && last_input.row==1);
    state.kind=View::kList; state.view++; state.navigation++; state.row_count=5;
    state.detail[0]=0;
    snprintf(state.title,sizeof state.title,"系统设置");
    const char* rows[]{"WiFi 网络","应用服务","设备信息","语言 / Language","封面同步"};
    for(int i=0;i<5;++i) snprintf(state.rows[i],112,"%s",rows[i]);
    SetScreen(state); Frame("sdgoods-settings.ppm");
    const auto coords=[](lv_obj_t* obj) { lv_area_t area; lv_obj_get_coords(obj,&area); return area; };
    const auto row=coords(rows_[0]); Tap(touch,(row.x1+row.x2)/2,(row.y1+row.y2)/2);
    assert(last_input.action==TouchAction::kRow && last_input.row==0);
    state.kind=View::kInfo; state.busy=true; state.progress=63; state.view++; state.navigation++;
    snprintf(state.title,sizeof state.title,"下载安装应用");
    snprintf(state.info,sizeof state.info,"1326 / 2105 KB");
    SetScreen(state); Frame("sdgoods-progress.ppm");
    const unsigned busy_clicks=clicks; Tap(touch,120,290); assert(clicks==busy_clicks);
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
    assert(strstr(lv_label_get_text(amoled_factory::wifi_url),"已安装"));
    state.battery_percent=72; state.wifi_connected=true;
    SetScreen(state); Frame("sdgoods-home.ppm");
    assert(lv_obj_has_flag(amoled_factory::wifi_title,LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(amoled_factory::count_label,LV_OBJ_FLAG_HIDDEN));
    language.store(Language::kEnglish);
    state.view++; SetScreen(state); Frame("sdgoods-home-en.ppm");
    language.store(Language::kChinese);
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
    state.view++; state.navigation++; SetScreen(state); Frame("sdgoods-home.ppm");
    touch.sample.x=180; touch.sample.y=325; touch.sample.pressed=true;
    clock_us+=40000; Render(nullptr); touch.sample.y=260; clock_us+=40000; Render(nullptr);
    assert(last_input.action==TouchAction::kHome);
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
    puts("SDGOODS: home tiles, four-cover grid, paging, empty state, actions, progress, settings and gestures passed.");
    return 0;
}
