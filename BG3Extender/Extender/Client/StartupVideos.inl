// The startup videos (StartupVideos.h): the composer, the declarations of the enabled mods and
// the rewrite of the game's startup playlist.
#include <Extender/Client/StartupVideos.h>
#include <Extender/Shared/StartupTimeline.h>
#include <CoreLib/JsonLibs.h>
#include <map>
#include <queue>
#include <tuple>

BEGIN_NS(ecl::startup_videos)

namespace
{
    // The game's playlist as ecl::GameStateStartLoading keeps it (eoc::GameOverPlaylistQueue at
    // state+0x18): one entry per load state, each the name of a playlist and how many of the flat
    // names belong to it (the entries' runs follow one another from Names[0]);
    // ecl::GameStateStartLoading::Enter makes one ecl::LoadStateMovie per entry.
    struct PlaylistEntry
    {
        FixedString Name;
        int32_t Count;
    };

    struct Playlist
    {
        Array<PlaylistEntry> Entries;
        Array<FixedString> Names;
        uint32_t ProgressTotal;
    };

    constexpr std::size_t PlaylistOffset = 0x18;
    constexpr std::size_t StateIdOffset = 0x10;
    constexpr uint32_t StartLoadingStateId = 22;
    constexpr char const* StartupVideoName = "Splash_Logo_Larian";
    constexpr char const* GameSource = "Game";

    StartupRecord gRecord;

    STDString ToSTD(StringView sv)
    {
        return STDString(sv.data(), sv.size());
    }

    STDString Lower(STDString s)
    {
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
        return s;
    }

    // ---- Ids: every rule about how an entry is named and referred to is here. ----

    constexpr char IdSeparator = ':';
    constexpr char const* GroupLabel = "*";

    // 8-4-4-4-12 hex digits: a mod's UUID; anything else before the colon is a ModTable.
    bool IsUuid(STDString const& s)
    {
        if (s.size() != 36) return false;
        for (std::size_t i = 0; i < s.size(); i++) {
            auto c = s[i];
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (c != '-') return false;
            } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        return true;
    }

    // The id a mod's Add entry is stored and returned under.
    STDString MakeModEntryId(STDString const& uuid, STDString const& label)
    {
        return uuid + IdSeparator + label;
    }

    // An Add's Id is a bare label: no colon, and not the group's "*".
    bool IsValidLabel(STDString const& label)
    {
        return !label.empty() && label.find(IdSeparator) == STDString::npos && label != GroupLabel;
    }

    // A reference as written: a bare name (a game entry), or <owner>:<label>, <owner>:*.
    struct ParsedRef
    {
        bool Game{ true };
        STDString Name;      // the game entry's name
        STDString Owner;     // UUID or ModTable
        STDString Label;     // the mod's label, or "*"
        bool Group{ false };
    };

    ParsedRef ParseRef(STDString const& ref)
    {
        ParsedRef out;
        auto colon = ref.find(IdSeparator);
        if (colon == STDString::npos) {
            out.Name = ref;
            return out;
        }

        out.Game = false;
        out.Owner = ref.substr(0, colon);
        out.Label = ref.substr(colon + 1);
        out.Group = (out.Label == GroupLabel);
        return out;
    }

    // ---- The composer. ----

    struct Node
    {
        STDString Id;
        STDString Video;
        // -1 for a game entry, otherwise the index of the declaration that added it.
        int Mod;
        bool Hidden{ false };
        int ReplacedBy{ -1 };
    };

    class Composer
    {
    public:
        Composer(std::vector<Declaration> const& declarations, std::vector<ModIdentity> const& mods, Composition& out)
            : decls_(declarations), out_(out)
        {
            for (auto const& d : declarations) AddKnownMod(d.Mod);
            for (auto const& m : mods) AddKnownMod(m);
        }

        void AddGameEntry(STDString const& name)
        {
            if (index_.find(name) != index_.end()) {
                Warn("The game's playlist names " + name + " twice; it is kept once");
                return;
            }
            Push(Node{ name, name, -1 });
        }

        void Run(bool enabled)
        {
            for (int mod = 0; mod < (int)decls_.size(); mod++) {
                AddEntries(mod);
            }

            for (int mod = 0; mod < (int)decls_.size(); mod++) {
                AddConstraints(mod);
            }

            auto order = Sort();

            for (int mod = 0; mod < (int)decls_.size(); mod++) {
                ApplyHide(mod);
                ApplyReplace(mod);
            }

            for (auto i : order) {
                auto const& node = nodes_[i];
                if (!enabled || node.Hidden) continue;
                ComposedEntry e;
                e.Id = node.Id;
                e.Video = node.Video;
                if (node.Mod < 0) {
                    e.Source = GameSource;
                } else {
                    e.SourceMod = decls_[node.Mod].Mod;
                    e.Source = e.SourceMod.Uuid;
                }
                if (node.ReplacedBy >= 0) e.ReplacedBy = decls_[node.ReplacedBy].Mod;
                out_.Entries.push_back(std::move(e));
            }
        }

    private:
        std::vector<Declaration> const& decls_;
        Composition& out_;
        // Every enabled mod a reference may name (the declarations' first), by UUID.
        std::vector<ModIdentity> known_;
        std::vector<Node> nodes_;
        std::map<STDString, int> index_;
        // after_[a] holds b when a must come before b.
        std::vector<std::vector<int>> after_;
        std::vector<int> inDegree_;
        // The Add entries that became nodes: (declaration, index in its Add, node).
        std::vector<std::tuple<int, std::size_t, int>> added_;

        void AddKnownMod(ModIdentity const& mod)
        {
            for (auto const& k : known_) {
                if (k.Uuid == mod.Uuid) return;
            }
            known_.push_back(mod);
        }

        void Warn(STDString const& msg)
        {
            out_.Warnings.push_back(msg);
        }

        STDString Who(int mod) const
        {
            return DescribeMod(decls_[mod].Mod);
        }

        void Push(Node&& node)
        {
            index_[node.Id] = (int)nodes_.size();
            nodes_.push_back(std::move(node));
            after_.emplace_back();
            inDegree_.push_back(0);
        }

        // The UUID a reference's owner names, or empty with `why` set.
        STDString ResolveOwner(STDString const& owner, STDString& why) const
        {
            if (IsUuid(owner)) {
                auto uuid = Lower(owner);
                for (auto const& k : known_) {
                    if (k.Uuid == uuid) return uuid;
                }
                why = "names no enabled mod with the UUID " + owner;
                return STDString();
            }

            std::vector<ModIdentity const*> found;
            for (auto const& k : known_) {
                if (!k.ModTable.empty() && k.ModTable == owner) found.push_back(&k);
            }

            if (found.empty()) {
                why = "names no enabled mod with the ModTable " + owner;
                return STDString();
            }

            if (found.size() > 1) {
                why = "names the ModTable " + owner + ", which " + DescribeMod(*found[0]) + " and "
                    + DescribeMod(*found[1]) + " both have";
                return STDString();
            }

            return found[0]->Uuid;
        }

        // The nodes a reference names: one entry, or every entry of a mod for <owner>:*.
        std::vector<int> Resolve(STDString const& ref, STDString& why) const
        {
            auto parsed = ParseRef(ref);
            if (parsed.Game) {
                auto it = index_.find(parsed.Name);
                if (it == index_.end() || nodes_[it->second].Mod >= 0) {
                    why = "names no entry of the game";
                    return {};
                }
                return { it->second };
            }

            auto uuid = ResolveOwner(parsed.Owner, why);
            if (uuid.empty()) return {};

            if (parsed.Group) {
                std::vector<int> members;
                for (int i = 0; i < (int)nodes_.size(); i++) {
                    if (nodes_[i].Mod >= 0 && decls_[nodes_[i].Mod].Mod.Uuid == uuid) members.push_back(i);
                }
                if (members.empty()) why = "names a mod that adds no entry";
                return members;
            }

            auto it = index_.find(MakeModEntryId(uuid, parsed.Label));
            if (it == index_.end()) {
                why = "names no entry " + parsed.Label + " of that mod";
                return {};
            }
            return { it->second };
        }

        void AddEntries(int mod)
        {
            auto const& decl = decls_[mod];
            for (std::size_t addIndex = 0; addIndex < decl.Add.size(); addIndex++) {
                auto const& add = decl.Add[addIndex];
                if (!IsValidLabel(add.Id)) {
                    Warn(Who(mod) + ": the Add entry \"" + add.Id + "\" is left out: its Id must be a bare label, without a colon, and not *");
                    continue;
                }

                auto id = MakeModEntryId(decl.Mod.Uuid, add.Id);
                if (add.Video.empty()) {
                    Warn(Who(mod) + ": the Add entry " + add.Id + " has no Video and is left out");
                    continue;
                }

                if (index_.find(id) != index_.end()) {
                    Warn(Who(mod) + ": the Add entry " + add.Id + " is added twice; the second is left out");
                    continue;
                }

                added_.push_back({ mod, addIndex, (int)nodes_.size() });
                Push(Node{ id, add.Video, mod });
            }
        }

        bool Reaches(int from, int to) const
        {
            std::vector<bool> seen(nodes_.size(), false);
            std::vector<int> stack{ from };
            while (!stack.empty()) {
                auto n = stack.back();
                stack.pop_back();
                if (n == to) return true;
                if (seen[n]) continue;
                seen[n] = true;
                for (auto next : after_[n]) {
                    stack.push_back(next);
                }
            }
            return false;
        }

        void Constrain(int mod, int self, STDString const& ref, bool before)
        {
            auto const& selfId = nodes_[self].Id;
            STDString word = before ? "Before" : "After";
            STDString why;
            auto targets = Resolve(ref, why);
            if (targets.empty()) {
                Warn(Who(mod) + ": " + selfId + " " + word + " " + ref + " " + why + "; the constraint is dropped");
                return;
            }

            std::vector<std::pair<int, int>> edges;
            for (auto target : targets) {
                if (target == self) continue;
                auto first = before ? self : target;
                auto second = before ? target : self;
                if (Reaches(second, first)) {
                    Warn(Who(mod) + ": " + selfId + " " + word + " " + ref + " would close a cycle ("
                        + nodes_[second].Id + " already comes before " + nodes_[first].Id
                        + "); this constraint, the latest loaded, is dropped");
                    return;
                }
                edges.push_back({ first, second });
            }

            if (edges.empty()) {
                Warn(Who(mod) + ": " + selfId + " " + word + " " + ref + " names only the entry itself; the constraint is dropped");
                return;
            }

            for (auto const& [first, second] : edges) {
                after_[first].push_back(second);
                inDegree_[second]++;
            }
        }

        void AddConstraints(int mod)
        {
            auto const& decl = decls_[mod];
            for (auto const& [addMod, addIndex, node] : added_) {
                if (addMod != mod) continue;
                auto const& add = decl.Add[addIndex];
                if (!add.Before.empty()) Constrain(mod, node, add.Before, true);
                if (!add.After.empty()) Constrain(mod, node, add.After, false);
            }
        }

        // A stable topological sort: of the entries whose predecessors are placed, the one first
        // in the base order (the game's, then load order and file order) goes next.
        std::vector<int> Sort()
        {
            std::vector<int> order;
            auto inDegree = inDegree_;
            std::priority_queue<int, std::vector<int>, std::greater<int>> ready;
            for (int i = 0; i < (int)nodes_.size(); i++) {
                if (inDegree[i] == 0) ready.push(i);
            }

            while (!ready.empty()) {
                auto n = ready.top();
                ready.pop();
                order.push_back(n);
                for (auto next : after_[n]) {
                    if (--inDegree[next] == 0) ready.push(next);
                }
            }

            return order;
        }

        // What a mod's Hide and Replace may reach: the game's entries, the entries of the mods
        // loaded before it and its own.
        bool Visible(int node, int mod) const
        {
            return nodes_[node].Mod <= mod;
        }

        void ApplyHide(int mod)
        {
            auto const& decl = decls_[mod];
            for (auto const& ref : decl.Hide) {
                if (ref == GroupLabel) {
                    for (auto& node : nodes_) {
                        if (node.Mod < mod) node.Hidden = true;
                    }
                    continue;
                }

                STDString why;
                auto targets = Resolve(ref, why);
                if (targets.empty()) {
                    Warn(Who(mod) + ": Hide " + ref + " " + why + "; nothing is hidden");
                    continue;
                }

                for (auto target : targets) {
                    if (!Visible(target, mod)) {
                        Warn(Who(mod) + ": Hide " + ref + " reaches " + nodes_[target].Id + ", added by a mod loaded later; it is not hidden");
                        continue;
                    }
                    nodes_[target].Hidden = true;
                }
            }
        }

        void ApplyReplace(int mod)
        {
            auto const& decl = decls_[mod];
            for (auto const& rep : decl.Replace) {
                if (rep.Video.empty()) {
                    Warn(Who(mod) + ": Replace " + rep.Id + " has no Video; nothing is replaced");
                    continue;
                }

                if (ParseRef(rep.Id).Group || rep.Id == GroupLabel) {
                    Warn(Who(mod) + ": Replace " + rep.Id + " names a group; Replace takes one entry, nothing is replaced");
                    continue;
                }

                STDString why;
                auto targets = Resolve(rep.Id, why);
                if (targets.empty()) {
                    Warn(Who(mod) + ": Replace " + rep.Id + " " + why + "; nothing is replaced");
                    continue;
                }

                auto& node = nodes_[targets[0]];
                if (!Visible(targets[0], mod)) {
                    Warn(Who(mod) + ": Replace " + rep.Id + " names an entry added by a mod loaded later; nothing is replaced");
                    continue;
                }

                if (node.ReplacedBy >= 0 && node.ReplacedBy != mod) {
                    Warn(Who(mod) + ": Replace " + node.Id + " overrides the Replace of " + Who(node.ReplacedBy)
                        + " (" + node.Video + " -> " + rep.Video + ")");
                }

                node.Video = rep.Video;
                node.ReplacedBy = mod;
                node.Hidden = false;
            }
        }
    };

    STDString GetString(rapidjson::Value const& obj, char const* key)
    {
        if (obj.IsObject()) {
            auto it = obj.FindMember(key);
            if (it != obj.MemberEnd() && it->value.IsString()) {
                return STDString(it->value.GetString(), it->value.GetStringLength());
            }
        }
        return STDString();
    }

    rapidjson::Value const* GetArray(rapidjson::Value const& obj, char const* key)
    {
        if (obj.IsObject()) {
            auto it = obj.FindMember(key);
            if (it != obj.MemberEnd() && it->value.IsArray()) {
                return &it->value;
            }
        }
        return nullptr;
    }

    // The ModTable of a mod's ScriptExtender/Config.json, empty when it names none.
    STDString ReadModTable(StringView json)
    {
        rapidjson::Document doc;
        doc.Parse(json.data(), json.size());
        if (doc.HasParseError()) return STDString();
        return GetString(doc, "ModTable");
    }
}

STDString DescribeMod(ModIdentity const& mod)
{
    STDString s = mod.Name.empty() ? STDString("a mod") : mod.Name;
    s += " (" + mod.Uuid;
    if (!mod.ModTable.empty()) s += ", ModTable " + mod.ModTable;
    s += ")";
    return s;
}

bool ParseDeclaration(ModIdentity const& mod, StringView json, Declaration& out, std::vector<STDString>& warnings)
{
    rapidjson::Document doc;
    doc.Parse(json.data(), json.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        warnings.push_back(DescribeMod(mod) + ": the declaration is not a JSON object; it is left out");
        return false;
    }

    out.Mod = mod;
    if (out.Mod.Uuid.empty()) out.Mod.Uuid = GetString(doc, "Uuid");
    if (out.Mod.ModTable.empty()) out.Mod.ModTable = GetString(doc, "ModTable");
    if (out.Mod.Name.empty()) out.Mod.Name = GetString(doc, "Name");
    if (out.Mod.Folder.empty()) out.Mod.Folder = GetString(doc, "Folder");
    out.Mod.Uuid = Lower(out.Mod.Uuid);
    if (!IsUuid(out.Mod.Uuid)) {
        warnings.push_back(DescribeMod(out.Mod) + ": a declaration without the mod's UUID is left out");
        return false;
    }

    if (auto hide = GetArray(doc, "Hide")) {
        for (auto const& v : hide->GetArray()) {
            if (v.IsString()) {
                out.Hide.push_back(STDString(v.GetString(), v.GetStringLength()));
            } else {
                warnings.push_back(DescribeMod(out.Mod) + ": a Hide item that is not a string is left out");
            }
        }
    }

    if (auto replace = GetArray(doc, "Replace")) {
        for (auto const& v : replace->GetArray()) {
            if (!v.IsObject()) {
                warnings.push_back(DescribeMod(out.Mod) + ": a Replace item that is not an object is left out");
                continue;
            }
            out.Replace.push_back(VideoRef{ GetString(v, "Id"), GetString(v, "Video") });
        }
    }

    if (auto add = GetArray(doc, "Add")) {
        for (auto const& v : add->GetArray()) {
            if (!v.IsObject()) {
                warnings.push_back(DescribeMod(out.Mod) + ": an Add item that is not an object is left out");
                continue;
            }
            out.Add.push_back(AddEntry{ GetString(v, "Id"), GetString(v, "Video"), GetString(v, "Before"), GetString(v, "After") });
        }
    }

    return true;
}

Composition Compose(std::vector<STDString> const& gameEntries, std::vector<Declaration> const& declarations,
    std::vector<ModIdentity> const& mods, bool enabled)
{
    Composition out;
    Composer composer(declarations, mods, out);
    for (auto const& name : gameEntries) {
        composer.AddGameEntry(name);
    }
    composer.Run(enabled);
    return out;
}

StartupRecord const& GetStartupRecord()
{
    return gRecord;
}

void OnStartLoading(void* state)
{
    if (state == nullptr || gRecord.Seen) return;

    auto id = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(state) + StateIdOffset);
    if (id != StartLoadingStateId) return;

    auto playlist = reinterpret_cast<Playlist*>(reinterpret_cast<uint8_t*>(state) + PlaylistOffset);
    // Only the first start's StartLoading carries the startup video; a new game's and a lobby's
    // carry no entry, and nothing is written into them.
    if (playlist->Entries.size() != 1
        || playlist->Names.size() != 1
        || playlist->Entries[0].Count != 1
        || playlist->Entries[0].Name.GetStringView() != StartupVideoName) {
        return;
    }

    gRecord.Seen = true;
    gRecord.Enabled = gExtender->GetConfig().StartupVideos;
    for (auto const& name : playlist->Names) {
        gRecord.GameEntries.push_back(ToSTD(name.GetStringView()));
    }

    std::vector<STDString> warnings;
    std::vector<ModIdentity> mods;
    auto modManager = GetStaticSymbols().GetModManagerClient();
    if (modManager != nullptr) {
        gRecord.SettingsMods = modManager->Settings.Mods.size();
        gRecord.AvailableMods = modManager->AvailableMods.size();
        gRecord.LoadOrderedModules = modManager->LoadOrderedModules.size();

        // modsettings.lsx's order is the load order.
        for (auto const& mod : modManager->Settings.Mods) {
            if (mod.Folder.empty()) continue;

            ModSeen seen;
            seen.Mod.Uuid = Lower(mod.ModuleUUIDString ? ToSTD(mod.ModuleUUIDString.GetStringView()) : mod.ModuleUUID.ToString());
            seen.Mod.Name = mod.Name;
            seen.Mod.Folder = mod.Folder;

            auto base = "Mods/" + mod.Folder;
            seen.MetaReadable = GetStaticSymbols().MakeFileReader(base + "/meta.lsx").IsLoaded();

            auto config = GetStaticSymbols().MakeFileReader(base + "/ScriptExtender/Config.json");
            if (config.IsLoaded()) {
                seen.ConfigReadable = true;
                seen.Mod.ModTable = ReadModTable(config.ToString());
            }

            auto reader = GetStaticSymbols().MakeFileReader(base + "/ScriptExtender/StartupVideos.json");
            if (reader.IsLoaded()) {
                seen.DeclarationFound = true;
                Declaration decl;
                if (ParseDeclaration(seen.Mod, reader.ToString(), decl, warnings)) {
                    gRecord.Declarations.push_back(std::move(decl));
                }
            }

            mods.push_back(seen.Mod);
            gRecord.Mods.push_back(std::move(seen));
        }
    } else {
        warnings.push_back("The client's mod manager is not available; no mod's declaration is read");
    }

    gRecord.Result = Compose(gRecord.GameEntries, gRecord.Declarations, mods, gRecord.Enabled);
    gRecord.Result.Warnings.insert(gRecord.Result.Warnings.begin(), warnings.begin(), warnings.end());

    // All videos as ONE entry whose run of names is the whole list. Enter gives each entry the run
    // Names[offset, offset + Count), offset being the sum of the earlier entries' counts, and makes
    // one ecl::LoadStateMovie of it; its Start hands the run to ecl::GameControl::ShowMovie, which
    // queues them, and the load state is ready only when the player stopped. So every video plays
    // before the state can exit (with one entry per video, StartLoading::CanExit let the game go on
    // after the first and the loading screen came over the rest), and one key press skips the rest
    // (VideoManager::Skip empties the queue). The entry is named after its first video, as
    // the game names an entry after its video. An empty list writes no entry: the game then makes no movie state.
    playlist->Entries.clear();
    playlist->Names.clear();
    for (auto const& entry : gRecord.Result.Entries) {
        playlist->Names.push_back(FixedString(entry.Video.c_str()));
    }
    if (!playlist->Names.empty()) {
        playlist->Entries.push_back(PlaylistEntry{ playlist->Names[0], (int32_t)playlist->Names.size() });
    }
    gRecord.Written = true;

    {
        char buf[64];
        if (playlist->Entries.size() == 1) {
            auto name = playlist->Entries[0].Name.GetStringView();
            snprintf(buf, sizeof(buf), "%.*s (%d names)", (int)name.size(), name.data(), playlist->Entries[0].Count);
        } else {
            snprintf(buf, sizeof(buf), "no entry");
        }
        startup_timeline::Record(startup_timeline::Side::Client, startup_timeline::What::PlaylistWritten, -1, -1, buf);
    }

    INFO("Startup videos: %d declarations, %d entries written, %d warnings%s",
        (int)gRecord.Declarations.size(), (int)gRecord.Result.Entries.size(),
        (int)gRecord.Result.Warnings.size(), gRecord.Enabled ? "" : " (StartupVideos is off)");
    for (auto const& w : gRecord.Result.Warnings) {
        WARN("Startup videos: %s", w.c_str());
    }
}

END_NS()
