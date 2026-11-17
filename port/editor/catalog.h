#ifndef CATALOG_H
#define CATALOG_H

enum Kind { K_OBJ, K_START, K_ATMOS, K_WILD };
enum Cat { C_ALIEN, C_MARINE, C_PRED, C_STRUCT, C_TREE, C_ROCK, C_PLANT, C_CRATE, C_DOODAD, C_ATMOS, C_START, C_WILD };

struct BaseLevel {
    const char *code;
    const char *name;
    int w, h;
};

static const BaseLevel k_bases[] = {
    { "M01", "M01 Desert", 230, 230 },
    { "A01", "A01", 205, 260 },
    { "A02", "A02", 190, 190 },
    { "A04", "A04", 129, 208 },
    { "A06", "A06", 270, 270 },
    { "A07", "A07", 216, 232 },
    { "A08", "A08", 320, 320 },
    { "M02", "M02", 213, 373 },
    { "M03", "M03", 144, 318 },
    { "M05", "M05", 200, 450 },
    { "M09", "M09", 200, 414 },
    { "M10", "M10", 200, 300 },
    { "P02", "P02", 259, 264 },
    { "P03", "P03", 244, 244 },
    { "P04", "P04", 379, 131 },
    { "P06", "P06", 200, 200 },
    { "P08", "P08", 240, 294 },
    { "P11", "P11", 284, 216 },
    { "T01", "T01", 200, 150 }
};
#define N_BASES ((int)(sizeof k_bases / sizeof k_bases[0]))

static const char *const k_alien[] = {
    "GAlienDrone", "GAlienRunner", "GAlienWarrior", "GAlienPraetorian", "GAlienRavager", "GAlienCarrier",
    "GQueen", "GEgg", "GHugger", "GChestBurster"
};
static const char *const k_marine[] = {
    "GMarineInfantry", "GMarineMedic", "GMarinePyro", "GMarineSadarTrooper", "GMarineSmartGunner",
    "GMarineSniper", "GMarineCommTech", "GSyntheticArmless"
};
static const char *const k_pred[] = {
    "GPredatorWarrior", "GPredatorBlooded", "GPredatorUnBlooded", "GPredatorBlazer", "GPredatorHydra",
    "GPredatorVanguard", "GPredatorSilentHunter", "GPredatorMasterHunter", "GPredatorAncient"
};
static const char *const k_struct[] = {
    "GHiveNode", "GEggSack", "GSkullKeeper", "GSentryGun", "GPredSentryGun", "GPredAntiAircraftGun",
    "GCloakerControlPod", "GCommLink", "GGenerator", "GAlarm", "GDoor", "GQueenTrap", "GControlPanelStand",
    "GExplosiveStructure", "GDropPod"
};
static const char *const k_doodad[] = {
    "@JoshTree2", "@Cactus1", "@Desertbush01", "@GenericRock01", "@GenericRock02", "@Crate01", "@Crate03",
    "@Militarycrate01", "@Militarycrate02", "@Militarycrate03", "@HCrate02", "@PredatorCrate", "@HoloGlobe1",
    "@DomeDebris1", "@Generic_RynthBones_1", "@Generic_RynthBones_2", "@Generic_RynthRibs"
};
static const char *const k_wild[] = { "GRynth", "GGroTye", "GKurn", "GKriltic", "GNiloc", "GRhiahn" };

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

#endif
