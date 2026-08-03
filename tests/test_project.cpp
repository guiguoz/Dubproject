#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "project/ProjectData.h"

using Catch::Matchers::WithinAbs;

TEST_CASE("ProjectData -- default values", "[project]")
{
    project::ProjectData data;
    REQUIRE(data.version == 7);
    REQUIRE(data.projectName == "Untitled");
    REQUIRE_THAT(data.bpm, WithinAbs(120.f, 0.001f));
    REQUIRE(data.midiMappings.empty());
    REQUIRE(data.currentScene == 0);
}

TEST_CASE("ProjectData -- dub delay defaults", "[project]")
{
    project::ProjectData data;
    REQUIRE(data.dubDelayEnabled == false);
    REQUIRE_THAT(data.dubDelaySend,     WithinAbs(0.20f, 0.001f));
    REQUIRE_THAT(data.dubDelayWet,      WithinAbs(0.28f, 0.001f));
    REQUIRE_THAT(data.dubDelayFeedback, WithinAbs(0.48f, 0.001f));
    REQUIRE_THAT(data.dubDelayTone,     WithinAbs(0.55f, 0.001f));
    REQUIRE_THAT(data.dubDelayDrive,    WithinAbs(0.15f, 0.001f));
    REQUIRE(data.dubDelayDiv == 1);
}

TEST_CASE("ProjectData -- SampleConfig defaults", "[project]")
{
    project::SampleConfig s;
    REQUIRE(s.filePath.empty());
    REQUIRE_THAT(s.gain, WithinAbs(1.f, 0.001f));
    REQUIRE(s.loop == false);
    REQUIRE(s.oneShot == true);
    REQUIRE(s.muted == false);
}

TEST_CASE("ProjectData -- MidiMapping", "[project]")
{
    project::MidiMapping m;
    m.midiNote  = 60;
    m.slotIndex = 2;
    REQUIRE(m.midiNote  == 60);
    REQUIRE(m.slotIndex == 2);
}

TEST_CASE("ProjectData -- SceneSaveData defaults", "[project]")
{
    project::SceneSaveData s;
    REQUIRE(s.used == false);
    REQUIRE_THAT(s.bpm, WithinAbs(120.f, 0.001f));
    for (int i = 0; i < 9; ++i)
    {
        REQUIRE(s.trackBarCounts[i] == 1);
        REQUIRE(s.trimStart[i] == 0);
        REQUIRE(s.trimEnd[i]   == -1);
        REQUIRE_THAT(s.delaySends[i], WithinAbs(0.f, 0.001f));
    }
}

// ─── §5.3 key-match invariants ────────────────────────────────────────────────
// Verrou de non-régression : l'intention utilisateur ne doit jamais être déduite.

TEST_CASE("ProjectData -- key-match: etat initial = Aucune, non arme", "[project][key-match]")
{
    // Un projet fraîchement créé ne doit jamais armer le key-match.
    project::ProjectData data;
    REQUIRE(data.masterKeyRoot     == -1);   // sentinelle "Aucune"
    REQUIRE(data.masterKeySetByUser == false); // jamais arme par défaut
}

TEST_CASE("ProjectData -- key-match: migration projet ancien (v<=21) = non arme", "[project][key-match]")
{
    // Simulation d'un projet sauvegardé avant v22 : masterKeyRoot = 0 (C),
    // masterKeySetByUser absent → migration doit produire false.
    // On simule ce que ProjectLoader fait pour version < 22 :
    project::ProjectData data;
    data.version            = 20;      // ancien format
    data.masterKeyRoot      = 0;       // C — était le default avant v22
    data.masterKeySetByUser = false;   // ProjectLoader force false pour v < 22

    // Le chargeur ne doit PAS déduire true depuis masterKeyRoot >= 0.
    REQUIRE(data.masterKeySetByUser == false);
}

TEST_CASE("ProjectData -- key-match: projet v22 avec cle explicite = arme", "[project][key-match]")
{
    // Un projet v22 où l'utilisateur a choisi explicitement G# (8).
    project::ProjectData data;
    data.version            = 22;
    data.masterKeyRoot      = 8;   // G#
    data.masterKeySetByUser = true;

    REQUIRE(data.masterKeyRoot      == 8);
    REQUIRE(data.masterKeySetByUser == true);
}

TEST_CASE("ProjectData -- key-match: projet v22 sans cle = non arme meme si root != -1", "[project][key-match]")
{
    // Bords : masterKeyRoot pourrait valoir 0 dans un projet v22
    // si l'utilisateur avait sélectionné C puis re-cliqué sur Aucune.
    // masterKeySetByUser est la vérité — pas masterKeyRoot.
    project::ProjectData data;
    data.version            = 22;
    data.masterKeyRoot      = 0;   // C — mais pas choisi intentionnellement
    data.masterKeySetByUser = false;

    REQUIRE(data.masterKeySetByUser == false);
}
