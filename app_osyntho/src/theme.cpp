#include "theme.h"

// Eight accent presets. The toolbar uses the accent as its background with white
// foreground in both light and dark themes, so darkBgColor == lightBgColor here;
// only the Material.theme (page background/controls) differs by type.
const std::array<Theme::Preset, 8> Theme::presets = {{
    {"#FFFFFFFF", "#FF607D8B", "#FF607D8B", "#FF607D8B"},  // blue grey
    {"#FFFFFFFF", "#FF673AB7", "#FF673AB7", "#FF673AB7"},  // deep purple
    {"#FFFFFFFF", "#FF009688", "#FF009688", "#FF009688"},  // teal
    {"#FFFFFFFF", "#FFFF5722", "#FFFF5722", "#FFFF5722"},  // deep orange
    {"#FFFFFFFF", "#FFE91E63", "#FFE91E63", "#FFE91E63"},  // pink
    {"#FFFFFFFF", "#FFFF8F00", "#FFFF8F00", "#FFFF8F00"},  // amber
    {"#FFFFFFFF", "#FF795548", "#FF795548", "#FF795548"},  // brown
    {"#FFFFFFFF", "#FF388E3C", "#FF388E3C", "#FF388E3C"},  // green
}};
