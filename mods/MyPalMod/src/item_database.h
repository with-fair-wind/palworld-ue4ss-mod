#pragma once
// Curated list of common Palworld item IDs (StaticItemId FName, bare — no prefix).
// Source: github.com/KURAMAAA0/PalModding ItemIDs.txt (verified against RequestAddItem).
// Not exhaustive — for the full list, type any ID in the Give field manually.

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
inline constexpr const char* kBrowseItems[] = {
    // --- Spheres ---
    "PalSphere", "PalSphere_Mega", "PalSphere_Giga", "PalSphere_Tera",
    "PalSphere_Master", "PalSphere_Legend", "PalSphere_Exotic", "PalSphere_Ultimate",
    // --- Basic materials ---
    "Stone", "Wood", "Fiber", "Coal", "CopperOre", "IronOre", "Sulfur", "Bone",
    "Horn", "PalFluid", "PalOil", "Cement", "Polymer", "CarbonFiber", "Plastic",
    "Quartz", "Silicon", "Sand", "Flint", "Charcoal", "Gunpowder", "Gunpowder2",
    "Leather", "Leather2", "Cloth", "MachineParts", "MachineParts2",
    // --- Ingots ---
    "CopperIngot", "IronIngot", "StealIngot", "Chromium", "StainlessSteel",
    // --- Money / valuables ---
    "Money", "DogCoin", "AncientParts2", "AncientParts3", "Ruby", "Sapphire",
    "Eemerald", "Diamond", "Relic", "RainbowCrystal",
    // --- Crystals / upgrades ---
    "Pal_crystal_S", "Pal_crystal_L", "PalUpgradeStone", "PalUpgradeStone2",
    "PalUpgradeStone3", "PalUpgradeStone4", "PalDarkParts", "NightStone",
    // --- Medicine / potions ---
    "Medicines", "LuxuryMedicines", "Potion_Low", "Potion", "Potion_High", "Potion_Extreme",
    "Antibiotic_Normal", "Antibiotic_Good", "Antibiotic_Super", "PalRevive",
    // --- Food ingredients ---
    "Berries", "Berries2", "Meat", "Meat2", "Egg", "Milk", "HotMilk", "Tomato",
    "Wheat", "Flour", "Lettuce", "Mushroom", "Honey", "Sulfur", "FishMeat",
    // --- Cooked dishes ---
    "Baked_Berries", "GrilledMeat", "BakedMeat_ChickenPal", "FriedEggs",
    "HotDog", "Hamburger", "Cheeseburger", "Pizza", "Cake", "Curry",
    // --- Eggs (Pal) ---
    "PalEgg_Normal_01", "PalEgg_Fire_01", "PalEgg_Water_01", "PalEgg_Leaf_01",
    "PalEgg_Electricity_01", "PalEgg_Ice_01", "PalEgg_Earth_01", "PalEgg_Dark_01",
    "PalEgg_Dragon_01",
    // --- Weapons ---
    "WeakerBow", "RecurveBow", "CompoundBow", "BowGun", "Musket",
    "HandGun_Default", "SingleShotRifle", "PumpActionShotgun", "DoubleBarrelShotgun",
    "AssaultRifle_Default1", "SniperRifle", "LaserRifle", "GatlingGun",
    "GrenadeLauncher", "FlameThrower", "RocketLauncher_NPC",
    // --- Armor ---
    "ClothArmor", "FurArmor", "CopperArmor", "IronArmor", "StealArmor", "PlasticArmor",
    "SFArmor", "ClothHat", "FurHelmet", "CopperHelmet", "IronHelmet", "StealHelmet",
    // --- Tools ---
    "Pickaxe_Tier_00", "Pickaxe_Tier_01", "Pickaxe_Tier_02", "Pickaxe_Tier_03",
    "Axe_Tier_00", "Axe_Tier_01", "Axe_Tier_02", "Axe_Tier_03",
    "Torch", "Glider_Old", "Glider_Good", "Glider_Super", "Glider_Legendary",
    "GrapplingGun", "WaterBucket", "FishingRod_Old", "RepairKit",
    // --- Bullets ---
    "Arrow", "SmallBullet", "LargeBullet", "HandgunBullet", "RifleBullet",
    "ShotgunBullet", "AssaultRifleBullet", "LaserBullet",
    // --- Books / skill / elixirs ---
    "TechnologyBook_G1", "TechnologyBook_G2", "TechnologyBook_G3",
    "ExpBoost_01", "ExpBoost_02", "Elixir_hp_01", "Elixir_attack_01",
    "Rankup_1", "Rankup_2", "Rankup_3",
    // --- Misc useful ---
    "SkillUnlock_Kitsunebi", "PalEgg_Normal_03", "PalEgg_Fire_03",
    "PalSphere_Robbery", "SphereLauncher", "MakeshiftHandgun",
    "Katana", "BeamSword", "OldRevolver", "SemiAutoShotgun",
};

inline constexpr int kBrowseItemCount = sizeof(kBrowseItems) / sizeof(kBrowseItems[0]);
// NOLINTEND(cppcoreguidelines-avoid-c-arrays)
