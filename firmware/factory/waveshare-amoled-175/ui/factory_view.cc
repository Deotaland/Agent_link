#include "factory_view.h"
#include "amoled_launcher_view.h"
#include "agent_cover.h"

namespace amoled_factory {
namespace {
AmoledLauncherView launcher_view;
void Create(lv_obj_t* root, AmoledFonts& fonts, void (*bind)(lv_obj_t*, int)) {
    launcher_view.Create(root, fonts, bind);
}
void Update(const amoled_ui::Screen& screen) {
    AmoledLauncherCard card;
    card.actions = screen.kind == amoled_ui::View::kAppActions;
    card.add = screen.add_card;
    card.index = screen.card_index;
    card.count = screen.card_count;
    card.image = agent_system::AcquireCover({screen.cover_slot, screen.cover_revision});
    card.name = screen.title;
    card.detail = screen.detail;
    card.app_id = screen.app_id;
    card.launch = amoled_ui::Tr("启动", "Launch");
    card.remove = amoled_ui::Tr("删除", "Delete");
    launcher_view.Update(card);
}
void Hide() {
    launcher_view.Hide();
}
}  // namespace
const amoled_ui::CardRenderer& Renderer() {
    static const amoled_ui::CardRenderer renderer{Create, Update, Hide};
    return renderer;
}
}  // namespace amoled_factory
