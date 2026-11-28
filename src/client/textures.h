// Backrooms Iris - procedural textures. Nothing is loaded from disk: every texture is
// painted in code, deterministic, and (where it repeats) tiles seamlessly.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../core/world.h"

namespace iris {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba; // w*h*4
    Image() = default;
    Image(int w_, int h_, uint32_t fill = 0xff000000u);
    void set(int x, int y, float r, float g, float b, float a = 1.0f);
    void blend(int x, int y, float r, float g, float b, float a); // alpha-over
    void rect(int x0, int y0, int x1, int y1, float r, float g, float b, float a = 1.0f);
    void disc(float cx, float cy, float rad, float r, float g, float b, float a = 1.0f, float soft = 1.0f);
    void line(float x0, float y0, float x1, float y1, float width, float r, float g, float b, float a = 1.0f);
    void text(int x, int y, int scale, const std::string& s, float r, float g, float b, float a = 1.0f);
};

// Surfaces a level is built from. The chunk mesher picks one per face.
enum Material : int {
    MAT_WALL,    // thin walls, posts, pillars
    MAT_FLOOR,   // main floor
    MAT_FLOOR2,  // alternate floor (puddles, road, parquet)
    MAT_FLOOR3,  // second alternate (sidewalk)
    MAT_CEIL,
    MAT_LIGHT,   // light fixtures (drawn full bright)
    MAT_BLOCK,   // sides of solid cells: crates, rock, hotel rooms, houses, buildings
    MAT_ROOF,    // tops of solid cells (outdoors / crates)
    MAT_DOOR,    // the exit door
    MAT_PROP,    // poles, frames
    MAT_FURN,    // furniture atlas (Level 5 rooms): 4 quadrants, see furniture_atlas()
    MAT_DECAL,   // alpha-tested detail atlas, 4x4: papers, stains, cracks, puddles, outlets... (see DECAL_*)
    MAT_CROP,    // alpha-tested, two-sided: wheat (Level 10)
    MAT_COUNT
};

// cells of the furniture/props atlas (MAT_FURN), 4x4
enum FurnTex : int {
    FT_WOOD, FT_SHEET, FT_VELVET, FT_DRAWER, FT_PAINTING, FT_METAL, FT_LOCKER, FT_CARDBOARD,
    FT_PLASTIC, FT_LEAVES, FT_BARK, FT_RUST, FT_FABRIC, FT_STONE, FT_GLOW, FT_RUBBER,
};

enum Decal : int {
    DECAL_PAPER, DECAL_CRUMPLED, DECAL_STAIN, DECAL_STAIN2, DECAL_CRACK, DECAL_PUDDLE, DECAL_LEAVES, DECAL_TRASH,
    DECAL_PRINTS, DECAL_MOLD, DECAL_SCUFF, DECAL_CAN, DECAL_OUTLET, DECAL_VENT, DECAL_STREAK, DECAL_GLASS,
    DECAL_COUNT
};

// size = base resolution (512 on PC, 256 on Vita)
void make_level_textures(LevelId level, int size, Image out[MAT_COUNT]);

// Sprites shared by every level
enum Sprite : int {
    SPR_WHITE,         // 4x4 white, for untextured coloured geometry
    SPR_FONT,          // pixel font atlas
    SPR_ALMOND, SPR_ENERGY, SPR_JUICE,  // item billboards / HUD icons
    SPR_EXIT_SIGN,
    SPR_SMILER, SPR_PARTY_FACE, SPR_SKIN,  // entity faces / skin
    SPR_SHADOW,        // shadow-person silhouette (hallucinations)
    SPR_SPIDER,        // crawls across your screen
    SPR_VIGNETTE,
    SPR_NOISE,
    SPR_GLOW,          // soft round glow
    SPR_BALLOON,       // party balloon (white, tinted)
    SPR_ROYAL, SPR_PAIN, SPR_CASHEW, SPR_LUCKY, SPR_FIRESALT, SPR_FLASHLIGHT, SPR_CROWBAR, // more items
    SPR_BEAM,          // flashlight cookie
    SPR_FLAME,         // burning firesalt
    SPR_CONE,          // light shaft: bright at the top, fading down and to the sides
    SPR_SCANLINES,     // camcorder look on the Vita (no shaders there)
    SPR_PLANET,        // a lit sphere (tinted per planet)
    SPR_SUPER, SPR_MOTH, SPR_SILENCE, SPR_MEDKIT, SPR_BULB, SPR_GLOWSTICK, SPR_MUSICBOX, SPR_RABBIT, SPR_COMPASS, SPR_RADIO,
    SPR_PISTOL, SPR_SHOTGUN, SPR_RIFLE,
    SPR_BATTERY, SPR_NV, SPR_CHALK, SPR_PILLS,
    SPR_STAR,          // a tiny bright point
    SPR_GRIT,          // near-white noise: surface grain for models
    SPR_SHELLS,        // 1.2: shotgun shells
    SPR_NOTE,          // 1.3: a handwritten note
    SPR_COUNT
};
Image make_sprite(Sprite s);
Sprite item_sprite(ItemType t);

} // namespace iris
