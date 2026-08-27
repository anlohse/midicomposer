#include <doctest/doctest.h>

#include "edit/edit_service.hpp"
#include "music/composition.hpp"
#include "project/project_document.hpp"

using namespace midi_composer;

namespace {

constexpr std::int64_t kPpqn = 480;
constexpr base::TrackId kTrack{1};

// A document with one empty track, which is all most edit operations need.
project::ProjectDocument make_document() {
    music::Composition comp{base::CompositionId{1}};
    comp.tracks().push_back(music::Track{kTrack, "Track"});
    return project::ProjectDocument{std::move(comp)};
}

const music::Track& track_of(const project::ProjectDocument& doc) {
    return doc.composition().tracks().front();
}

// EditService never invents note ids; DocumentManager supplies them in
// production. Tests must do the same, or every note lands on id 0 and lookups
// find the wrong one.
struct Ids {
    std::uint64_t next{1};
    base::NoteId operator()() { return base::NoteId{next++}; }
};

std::vector<std::int64_t> note_starts(const project::ProjectDocument& doc) {
    std::vector<std::int64_t> out;
    for (const auto& n : track_of(doc).notes()) out.push_back(n.start.value());
    return out;
}

// Kinds of the changes a mutation recorded, so the patch stream can be asserted on.
std::vector<project::ChangeKind> drain_kinds(project::ProjectDocument& doc) {
    std::vector<project::ChangeKind> out;
    for (const auto& c : doc.take_pending_changes()) out.push_back(c.kind);
    return out;
}

} // namespace

TEST_CASE("creating a note records it and undo removes it") {
    auto doc = make_document();
    edit::EditService svc;

    Ids ids;
    auto id = svc.create_note(doc, kTrack, timeline::Tick{0}, timeline::TickDuration{kPpqn}, 60, 100, ids());
    REQUIRE(id.has_value());
    CHECK(track_of(doc).notes().size() == 1);
    CHECK(doc.revision() == 1);
    CHECK(doc.dirty());
    CHECK(drain_kinds(doc) == std::vector{project::ChangeKind::NoteCreated});

    REQUIRE(doc.history().undo());
    CHECK(track_of(doc).notes().empty());
    // Undo replays the same helpers, so it reports the inverse change.
    CHECK(drain_kinds(doc) == std::vector{project::ChangeKind::NoteDeleted});

    REQUIRE(doc.history().redo());
    CHECK(track_of(doc).notes().size() == 1);
    CHECK(drain_kinds(doc) == std::vector{project::ChangeKind::NoteCreated});
}

TEST_CASE("notes stay sorted by start tick") {
    auto doc = make_document();
    edit::EditService svc;
    Ids ids;
    for (auto tick : {kPpqn * 2, std::int64_t{0}, kPpqn}) {
        REQUIRE(svc.create_note(doc, kTrack, timeline::Tick{tick},
                                timeline::TickDuration{kPpqn}, 60, 100, ids()).has_value());
    }
    CHECK(note_starts(doc) == std::vector<std::int64_t>{0, kPpqn, kPpqn * 2});
}

TEST_CASE("moving a note re-sorts and reports a delete plus a create") {
    auto doc = make_document();
    edit::EditService svc;
    Ids ids;
    auto first = svc.create_note(doc, kTrack, timeline::Tick{0}, timeline::TickDuration{kPpqn}, 60, 100, ids());
    REQUIRE(first.has_value());
    REQUIRE(svc.create_note(doc, kTrack, timeline::Tick{kPpqn}, timeline::TickDuration{kPpqn}, 62, 100, ids()).has_value());
    drain_kinds(doc);

    REQUIRE(svc.move_note(doc, kTrack, *first, timeline::Tick{kPpqn * 3}).has_value());
    CHECK(note_starts(doc) == std::vector<std::int64_t>{kPpqn, kPpqn * 3});
    // Re-sorting is an erase plus an insert, so that is what the UI is told.
    CHECK(drain_kinds(doc) == std::vector{project::ChangeKind::NoteDeleted,
                                         project::ChangeKind::NoteCreated});

    REQUIRE(doc.history().undo());
    CHECK(note_starts(doc) == std::vector<std::int64_t>{0, kPpqn});
}

TEST_CASE("resize and update report the note's new state") {
    auto doc = make_document();
    edit::EditService svc;
    Ids ids;
    auto id = svc.create_note(doc, kTrack, timeline::Tick{0}, timeline::TickDuration{kPpqn}, 60, 100, ids());
    REQUIRE(id.has_value());
    drain_kinds(doc);

    REQUIRE(svc.resize_note(doc, kTrack, *id, timeline::TickDuration{240}).has_value());
    CHECK(track_of(doc).notes().front().duration.value() == 240);
    CHECK(drain_kinds(doc) == std::vector{project::ChangeKind::NoteUpdated});

    REQUIRE(svc.update_note(doc, kTrack, *id, std::uint8_t{72}, std::uint8_t{55}).has_value());
    CHECK(track_of(doc).notes().front().pitch == 72);
    CHECK(track_of(doc).notes().front().velocity == 55);

    REQUIRE(doc.history().undo());
    CHECK(track_of(doc).notes().front().pitch == 60);
    CHECK(track_of(doc).notes().front().velocity == 100);
}

TEST_CASE("a no-op update neither bumps the revision nor records a change") {
    auto doc = make_document();
    edit::EditService svc;
    Ids ids;
    auto id = svc.create_note(doc, kTrack, timeline::Tick{0}, timeline::TickDuration{kPpqn}, 60, 100, ids());
    REQUIRE(id.has_value());
    drain_kinds(doc);
    const auto revision = doc.revision();

    REQUIRE(svc.update_note(doc, kTrack, *id, std::uint8_t{60}, std::uint8_t{100}).has_value());
    CHECK(doc.revision() == revision);
    CHECK(drain_kinds(doc).empty());
}

TEST_CASE("invalid notes are rejected") {
    auto doc = make_document();
    edit::EditService svc;

    Ids ids;
    CHECK_FALSE(svc.create_note(doc, kTrack, timeline::Tick{-1}, timeline::TickDuration{kPpqn}, 60, 100, ids()));
    CHECK_FALSE(svc.create_note(doc, kTrack, timeline::Tick{0}, timeline::TickDuration{0}, 60, 100, ids()));
    CHECK_FALSE(svc.create_note(doc, kTrack, timeline::Tick{0}, timeline::TickDuration{kPpqn}, 128, 100, ids()));
    // Velocity 0 would be a note-off on the wire.
    CHECK_FALSE(svc.create_note(doc, kTrack, timeline::Tick{0}, timeline::TickDuration{kPpqn}, 60, 0, ids()));
    CHECK_FALSE(svc.create_note(doc, base::TrackId{99}, timeline::Tick{0},
                                timeline::TickDuration{kPpqn}, 60, 100, ids()));

    CHECK(track_of(doc).notes().empty());
    CHECK(doc.revision() == 0);
}

TEST_CASE("a batch edit is one undo step") {
    auto doc = make_document();
    edit::EditService svc;

    std::vector<edit::BatchOperation> ops;
    for (int i = 0; i < 3; ++i) {
        edit::BatchOperation op;
        op.type = edit::BatchOperation::Type::CreateNote;
        op.track_id = kTrack;
        op.note_id = base::NoteId{static_cast<std::uint64_t>(i + 1)};
        op.start = timeline::Tick{i * kPpqn};
        op.duration = timeline::TickDuration{kPpqn};
        op.pitch = static_cast<std::uint8_t>(60 + i);
        op.velocity = 100;
        ops.push_back(op);
    }

    auto created = svc.batch_edit(doc, ops);
    REQUIRE(created.has_value());
    CHECK(created->size() == 3);
    CHECK(track_of(doc).notes().size() == 3);
    CHECK(doc.revision() == 1);

    REQUIRE(doc.history().undo());
    CHECK(track_of(doc).notes().empty());
    REQUIRE(doc.history().redo());
    CHECK(track_of(doc).notes().size() == 3);
}

TEST_CASE("a failing batch rolls back and records nothing") {
    auto doc = make_document();
    edit::EditService svc;
    Ids ids;
    REQUIRE(svc.create_note(doc, kTrack, timeline::Tick{0}, timeline::TickDuration{kPpqn}, 60, 100, ids()).has_value());
    drain_kinds(doc);
    const auto revision = doc.revision();

    std::vector<edit::BatchOperation> ops;
    edit::BatchOperation ok;
    ok.type = edit::BatchOperation::Type::CreateNote;
    ok.track_id = kTrack;
    ok.note_id = base::NoteId{50};
    ok.start = timeline::Tick{kPpqn};
    ok.duration = timeline::TickDuration{kPpqn};
    ok.pitch = 64;
    ok.velocity = 100;
    ops.push_back(ok);

    edit::BatchOperation bad;                      // moving a note that does not exist
    bad.type = edit::BatchOperation::Type::MoveNote;
    bad.track_id = kTrack;
    bad.note_id = base::NoteId{9999};
    bad.start = timeline::Tick{0};
    ops.push_back(bad);

    CHECK_FALSE(svc.batch_edit(doc, ops));
    CHECK(track_of(doc).notes().size() == 1);      // the good op was rolled back
    CHECK(doc.revision() == revision);
    // Whatever the rollback recorded describes state that no longer exists, so the
    // facade discards it; here we just confirm the revision never moved, which is
    // the signal it uses.
    CHECK(doc.history().can_undo() == true);       // only the earlier create
    doc.take_pending_changes();
}

TEST_CASE("track properties are undoable") {
    auto doc = make_document();
    edit::EditService svc;

    REQUIRE(svc.rename_track(doc, kTrack, "Lead").has_value());
    CHECK(track_of(doc).name() == "Lead");
    REQUIRE(svc.set_track_channel(doc, kTrack, 5).has_value());
    CHECK(track_of(doc).midi_channel() == 5);
    REQUIRE(svc.set_track_clef(doc, kTrack, music::Clef::Bass8vb).has_value());
    CHECK(track_of(doc).clef() == music::Clef::Bass8vb);

    REQUIRE(doc.history().undo());
    CHECK(track_of(doc).clef() == music::Clef::Treble);
    REQUIRE(doc.history().undo());
    CHECK(track_of(doc).midi_channel() == 0);
    REQUIRE(doc.history().undo());
    CHECK(track_of(doc).name() == "Track");
}

TEST_CASE("an out-of-range channel is rejected") {
    auto doc = make_document();
    edit::EditService svc;
    CHECK_FALSE(svc.set_track_channel(doc, kTrack, 16));
    CHECK(track_of(doc).midi_channel() == 0);
}

TEST_CASE("the instrument is the program change at tick 0") {
    auto doc = make_document();
    edit::EditService svc;

    REQUIRE(svc.set_track_program(doc, kTrack, 40).has_value());
    REQUIRE(track_of(doc).program_changes().size() == 1);
    CHECK(track_of(doc).program_changes().front().tick.value() == 0);
    CHECK(track_of(doc).program_changes().front().program == 40);

    // Choosing again updates that event rather than adding a second one.
    REQUIRE(svc.set_track_program(doc, kTrack, 56).has_value());
    REQUIRE(track_of(doc).program_changes().size() == 1);
    CHECK(track_of(doc).program_changes().front().program == 56);

    REQUIRE(doc.history().undo());
    CHECK(track_of(doc).program_changes().front().program == 40);
    // Undoing the insertion removes the event entirely.
    REQUIRE(doc.history().undo());
    CHECK(track_of(doc).program_changes().empty());
}

TEST_CASE("controller events are created in tick order and are undoable") {
    auto doc = make_document();
    edit::EditService svc;

    // Deliberately out of order: the service sorts, so nothing downstream has to.
    auto later = svc.create_controller_event(doc, kTrack, timeline::Tick{kPpqn}, 7, 100);
    auto first = svc.create_controller_event(doc, kTrack, timeline::Tick{0}, 1, 64);
    REQUIRE(later.has_value());
    REQUIRE(first.has_value());
    CHECK(*later != *first);                       // fresh id per event

    const auto& ccs = track_of(doc).controller_events();
    REQUIRE(ccs.size() == 2);
    CHECK(ccs[0].tick.value() == 0);
    CHECK(ccs[0].controller == 1);
    CHECK(ccs[1].tick.value() == kPpqn);
    // One record per committed operation. Each carries the whole list, so the
    // later one supersedes the earlier when the UI applies them in order.
    CHECK(drain_kinds(doc) == std::vector{project::ChangeKind::TrackControllersUpdated,
                                          project::ChangeKind::TrackControllersUpdated});

    REQUIRE(doc.history().undo());
    CHECK(track_of(doc).controller_events().size() == 1);
    REQUIRE(doc.history().redo());
    CHECK(track_of(doc).controller_events().size() == 2);
}

TEST_CASE("a controller update touches only the fields it is given") {
    auto doc = make_document();
    edit::EditService svc;
    auto id = svc.create_controller_event(doc, kTrack, timeline::Tick{0}, 7, 100);
    REQUIRE(id.has_value());

    REQUIRE(svc.update_controller_event(doc, kTrack, *id, std::nullopt, std::nullopt,
                                        std::uint8_t{20}).has_value());
    CHECK(track_of(doc).controller_events().front().controller == 7);   // untouched
    CHECK(track_of(doc).controller_events().front().value == 20);

    // Moving one re-sorts the list, which is why the whole list is shipped.
    REQUIRE(svc.create_controller_event(doc, kTrack, timeline::Tick{kPpqn}, 10, 64).has_value());
    REQUIRE(svc.update_controller_event(doc, kTrack, *id, timeline::Tick{kPpqn * 2},
                                        std::nullopt, std::nullopt).has_value());
    CHECK(track_of(doc).controller_events().back().id == *id);

    REQUIRE(doc.history().undo());
    CHECK(track_of(doc).controller_events().front().id == *id);
}

TEST_CASE("a no-op controller update records nothing") {
    auto doc = make_document();
    edit::EditService svc;
    auto id = svc.create_controller_event(doc, kTrack, timeline::Tick{0}, 7, 100);
    REQUIRE(id.has_value());
    drain_kinds(doc);
    const auto revision = doc.revision();

    REQUIRE(svc.update_controller_event(doc, kTrack, *id, timeline::Tick{0},
                                        std::uint8_t{7}, std::uint8_t{100}).has_value());
    CHECK(doc.revision() == revision);
    CHECK(drain_kinds(doc).empty());
}

TEST_CASE("out-of-range controller data is rejected") {
    auto doc = make_document();
    edit::EditService svc;
    CHECK_FALSE(svc.create_controller_event(doc, kTrack, timeline::Tick{-1}, 7, 100));
    CHECK_FALSE(svc.create_controller_event(doc, kTrack, timeline::Tick{0}, 128, 100));
    CHECK_FALSE(svc.create_controller_event(doc, kTrack, timeline::Tick{0}, 7, 128));
    CHECK_FALSE(svc.create_controller_event(doc, base::TrackId{99}, timeline::Tick{0}, 7, 100));
    CHECK(track_of(doc).controller_events().empty());
    CHECK(doc.revision() == 0);
}

TEST_CASE("deleting a controller event restores on undo") {
    auto doc = make_document();
    edit::EditService svc;
    auto id = svc.create_controller_event(doc, kTrack, timeline::Tick{0}, 7, 100);
    REQUIRE(id.has_value());

    REQUIRE(svc.delete_controller_event(doc, kTrack, *id).has_value());
    CHECK(track_of(doc).controller_events().empty());
    CHECK_FALSE(svc.delete_controller_event(doc, kTrack, *id));   // already gone

    REQUIRE(doc.history().undo());
    REQUIRE(track_of(doc).controller_events().size() == 1);
    CHECK(track_of(doc).controller_events().front().value == 100);
}

TEST_CASE("pitch bends accept the full signed range and nothing outside it") {
    auto doc = make_document();
    edit::EditService svc;

    auto low = svc.create_pitch_bend(doc, kTrack, timeline::Tick{0}, -8192);
    auto high = svc.create_pitch_bend(doc, kTrack, timeline::Tick{kPpqn}, 8191);
    REQUIRE(low.has_value());
    REQUIRE(high.has_value());
    CHECK(track_of(doc).pitch_bends().size() == 2);
    CHECK(drain_kinds(doc) == std::vector{project::ChangeKind::TrackPitchBendsUpdated,
                                          project::ChangeKind::TrackPitchBendsUpdated});

    CHECK_FALSE(svc.create_pitch_bend(doc, kTrack, timeline::Tick{0}, -8193));
    CHECK_FALSE(svc.create_pitch_bend(doc, kTrack, timeline::Tick{0}, 8192));
    CHECK_FALSE(svc.create_pitch_bend(doc, kTrack, timeline::Tick{-1}, 0));
    CHECK(track_of(doc).pitch_bends().size() == 2);

    REQUIRE(svc.update_pitch_bend(doc, kTrack, *low, std::nullopt, std::int16_t{4096}).has_value());
    CHECK(track_of(doc).pitch_bends().front().value == 4096);
    CHECK_FALSE(svc.update_pitch_bend(doc, kTrack, *low, std::nullopt, std::int16_t{9000}));
    CHECK(track_of(doc).pitch_bends().front().value == 4096);

    REQUIRE(svc.delete_pitch_bend(doc, kTrack, *high).has_value());
    CHECK(track_of(doc).pitch_bends().size() == 1);
    REQUIRE(doc.history().undo());
    CHECK(track_of(doc).pitch_bends().size() == 2);
}

TEST_CASE("event ids do not collide across the kinds sharing the counter") {
    auto doc = make_document();
    edit::EditService svc;

    auto cc = svc.create_controller_event(doc, kTrack, timeline::Tick{0}, 7, 100);
    auto pb = svc.create_pitch_bend(doc, kTrack, timeline::Tick{0}, 1000);
    REQUIRE(svc.set_track_program(doc, kTrack, 40).has_value());
    REQUIRE(cc.has_value());
    REQUIRE(pb.has_value());

    // One counter is shared by every event kind, and the panel addresses events
    // by id, so a collision would make one row edit another.
    CHECK(*cc != *pb);
    CHECK(track_of(doc).program_changes().front().id != *cc);
    CHECK(track_of(doc).program_changes().front().id != *pb);
}

TEST_CASE("deleting a track and undoing restores its notes") {
    auto doc = make_document();
    edit::EditService svc;
    Ids ids;
    REQUIRE(svc.create_note(doc, kTrack, timeline::Tick{0}, timeline::TickDuration{kPpqn}, 48, 90, ids()).has_value());

    REQUIRE(svc.delete_track(doc, kTrack).has_value());
    CHECK(doc.composition().tracks().empty());

    REQUIRE(doc.history().undo());
    REQUIRE(doc.composition().tracks().size() == 1);
    CHECK(track_of(doc).notes().size() == 1);
    CHECK(track_of(doc).notes().front().pitch == 48);
}
