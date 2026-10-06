#include "lineup/practice_internal.h"
#include <iostream>
using Json = nlohmann::json;
void check(bool result, const char *message) { if (!result) throw std::runtime_error(message); }
int main() {
    auto root = std::filesystem::temp_directory_path() / ("xen-practice-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        Json recipe = {{"id", "r1"}, {"draft", false}, {"reference_id", "ref-v1"}, {"conditions", "default"}, {"team", "T"}, {"map", "A"}};
        Json favorite = {{"action", "favorite"}, {"id", "r1"}, {"enabled", true}, {"request_id", "fav1"}};
        Json record = {{"action", "practice_record"}, {"id", "r1"}, {"outcome", "success"}, {"conditions", "manual condition"}, {"request_id", "attempt1"}};
        {
            lineup::detail::PracticeStore store(root);
            check(store.apply(favorite, recipe, Json::object()), "favorite");
            check(!store.apply(favorite, recipe, Json::object()), "favorite duplicate");
            check(store.apply({{"action","practice_queue"},{"id","r1"},{"enabled",true},{"request_id","q1"}}, recipe, {{"map","A"},{"team","T"}}), "queue");
            check(store.apply(record, recipe, {{"map","A"},{"team","T"}}), "record");
        }
        {
            lineup::detail::PracticeStore store(root);
            auto state = store.snapshot();
            check(state["favorites"] == Json::array({"r1"}) && state["queue"] == Json::array({"r1"}), "restore selection");
            check(!store.apply(record, recipe, Json::object()) && state["history_count"] == 1, "persistent duplicate");
            check(state["history"][0]["recipe"]["reference_id"] == "ref-v1", "reference identity");
            check(state["history"][0]["command"]["conditions"] == "manual condition", "conditions identity");
            check(state["history"][0]["context"]["team"] == "T", "context identity");
            auto conflict = record; conflict["outcome"] = "failure";
            bool rejected = false; try { store.apply(conflict, recipe, {}); } catch (...) { rejected = true; }
            check(rejected, "duplicate payload conflict");
            auto invalid = record; invalid["request_id"] = "invalid"; invalid["outcome"] = "automatic";
            rejected = false; try { store.apply(invalid, recipe, {}); } catch (...) { rejected = true; }
            check(rejected, "reject automatic judgement");
            recipe["reference_id"] = "ref-v2";
            for (int i = 0; i < 102; ++i) { auto next=record; next["request_id"]="next"+std::to_string(i); next["outcome"]=i%2 ? "skip" : "failure"; store.apply(next,recipe,Json::object()); }
            check(store.snapshot()["history"].size() == 100 && store.snapshot()["history_count"] == 103, "bounded response preserves total");
            favorite["request_id"]="fav2"; favorite["enabled"]=false; store.apply(favorite,recipe,{});
            check(store.snapshot()["favorites"].empty() && store.snapshot()["queue"].size()==1,"independent membership");
        }
        // 未发布临时文件不成为记录；原始已发布事件仍包含旧参考。
        std::ofstream(root/"practice-events"/"interrupted.tmp") << "partial";
        lineup::detail::PracticeStore restored(root);
        check(restored.snapshot()["history_count"]==103 && restored.snapshot()["error"]=="", "partial temp ignored");
        Json original; std::ifstream(root/"practice-events"/"00000000000000000003.json") >> original;
        check(original["recipe"]["reference_id"]=="ref-v1", "old history immutable");
        // 合法 JSON 但非法人工结果也必须被拒绝。
        original["command"]["outcome"]="automatic";
        std::ofstream(root/"practice-events"/"00000000000000000003.json") << original;
        lineup::detail::PracticeStore invalid_outcome(root);
        check(!invalid_outcome.snapshot()["error"].get<std::string>().empty(), "invalid outcome visible");
        std::ofstream(root/"practice-events"/"99999999999999999999.json") << "broken";
        lineup::detail::PracticeStore broken(root);
        check(!broken.snapshot()["error"].get<std::string>().empty(), "corruption visible");
        bool rejected=false; try { broken.apply(record,recipe,{}); } catch (...) { rejected=true; }
        check(rejected,"corruption blocks only journal writes");
        std::cout << "practice persistence, dedupe, history and recovery passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
