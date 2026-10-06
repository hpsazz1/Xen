#ifndef XEN_LINEUP_PRACTICE_INTERNAL_H
#define XEN_LINEUP_PRACTICE_INTERNAL_H
// 独立追加日志：不重写参考、目录或既有视觉 observations；由 Service mutex 串行调用。
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace lineup::detail {
class PracticeStore {
    using Json = nlohmann::json;
    std::filesystem::path directory_;
    Json favorites_ = Json::array(), queue_ = Json::array(), history_ = Json::array();
    std::map<std::string, Json> requests_;
    std::uint64_t sequence_ = 0, history_count_ = 0;
    std::string error_;
    static void membership(Json &values, const std::string &id, bool enabled) {
        auto found = std::find(values.begin(), values.end(), Json(id));
        if (enabled && found == values.end()) values.push_back(id);
        if (!enabled && found != values.end()) values.erase(found);
    }
    void replay(const Json &event) {
        const auto &command = event.at("command");
        auto action = command.at("action").get<std::string>();
        auto id = command.at("id").get<std::string>();
        auto key = command.at("request_id").get<std::string>();
        if (event.at("schema") != 1 || event.at("sequence") != sequence_ + 1 || requests_.contains(key))
            throw std::runtime_error("practice journal sequence invalid");
        if (id.empty() || key.empty() || key.size() > 128 || !event.at("recipe").is_object() ||
            event.at("recipe").at("id") != id || !event.at("context").is_object() ||
            !event.at("recorded_at_unix_ms").is_number_integer() || event.at("judgement") != "manual")
            throw std::runtime_error("practice journal fields invalid");
        if (action == "practice_record") {
            auto outcome = command.at("outcome").get<std::string>();
            if ((outcome != "success" && outcome != "failure" && outcome != "skip") ||
                !command.at("conditions").is_string() || command.at("conditions").get<std::string>().size() > 2000)
                throw std::runtime_error("practice journal outcome invalid");
        }
        if (action == "favorite") membership(favorites_, id, command.at("enabled").get<bool>());
        else if (action == "practice_queue") membership(queue_, id, command.at("enabled").get<bool>());
        else if (action == "practice_record") {
            history_.push_back(event);
            ++history_count_;
            if (history_.size() > 100) history_.erase(history_.begin());
        } else throw std::runtime_error("practice journal action invalid");
        requests_.emplace(key, command);
        ++sequence_;
    }
  public:
    explicit PracticeStore(const std::filesystem::path &root) : directory_(root / "practice-events") {
        try {
            if (!std::filesystem::exists(directory_)) return;
            std::vector<std::filesystem::path> files;
            for (auto &entry : std::filesystem::directory_iterator(directory_))
                if (entry.path().extension() == ".json") files.push_back(entry.path());
            std::sort(files.begin(), files.end());
            for (auto &path : files) { std::ifstream input(path); Json event; input >> event; replay(event); }
        } catch (const std::exception &e) { error_ = e.what(); }
    }
    Json snapshot() const {
        return {{"favorites", favorites_}, {"queue", queue_}, {"history", history_},
                {"history_count", history_count_}, {"error", error_}, {"manual_only", true}};
    }
    // 返回 false 表示已持久化的同一请求；冲突或损坏禁止续写，保留原日志供恢复。
    bool apply(const Json &input, const Json &recipe, const Json &context) {
        if (!error_.empty()) throw std::runtime_error("practice journal unavailable: " + error_);
        const auto action = input.at("action").get<std::string>();
        const auto id = input.at("id").get<std::string>();
        const auto key = input.at("request_id").get<std::string>();
        if (key.empty() || key.size() > 128 || id.empty() || recipe.at("id") != id || recipe.value("draft", true))
            throw std::invalid_argument("invalid practice request");
        Json command = {{"action", action}, {"id", id}, {"request_id", key}};
        if (action == "favorite" || action == "practice_queue") command["enabled"] = input.at("enabled").get<bool>();
        else if (action == "practice_record") {
            auto outcome = input.at("outcome").get<std::string>();
            if (outcome != "success" && outcome != "failure" && outcome != "skip") throw std::invalid_argument("invalid manual outcome");
            auto conditions = input.value("conditions", "");
            if (conditions.size() > 2000) throw std::invalid_argument("practice conditions too long");
            command["outcome"] = outcome; command["conditions"] = conditions;
        } else throw std::invalid_argument("unknown practice action");
        if (auto found = requests_.find(key); found != requests_.end()) {
            if (found->second != command) throw std::invalid_argument("practice request id conflict");
            return false;
        }
        Json event = {{"schema", 1}, {"sequence", sequence_ + 1}, {"command", command},
                      {"recorded_at_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
                      {"recipe", recipe}, {"context", context.is_object() ? context : Json::object()}, {"judgement", "manual"}};
        std::filesystem::create_directories(directory_);
        std::ostringstream name; name << std::setw(20) << std::setfill('0') << sequence_ + 1;
        auto destination = directory_ / (name.str() + ".json"), temporary = directory_ / (name.str() + ".tmp");
        if (std::filesystem::exists(destination)) throw std::runtime_error("practice journal collision");
        { std::ofstream output(temporary, std::ios::binary | std::ios::trunc); output << event.dump(2); output.flush();
          if (!output) throw std::runtime_error("practice journal write failed"); output.close();
          if (!output) throw std::runtime_error("practice journal close failed"); }
        std::filesystem::rename(temporary, destination);
        replay(event);
        return true;
    }
};
} // namespace lineup::detail
#endif
