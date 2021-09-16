#if 1
#  include "MEM_guardedalloc.h"

#  include "BLI_alloca.h"
#  include "BLI_array.h"
#  include "BLI_bitmap.h"
#  include "BLI_compiler_attrs.h"
#  include "BLI_compiler_compat.h"
#  include "BLI_listbase.h"
#  include "BLI_math.h"
#  include "BLI_memarena.h"

#  include "DNA_brush_enums.h"
#  include "DNA_brush_types.h"
#  include "DNA_color_types.h"
#  include "DNA_curveprofile_types.h"
#  include "DNA_node_types.h"

#  include "BKE_brush.h"
#  include "BKE_colorband.h"
#  include "BKE_colortools.h"
#  include "BKE_context.h"
#  include "BKE_node.h"
#  include "BKE_paint.h"

#  include "BKE_curveprofile.h"

#  define MAX_BRUSH_COMMAND_PARAMS 16
#  define MAX_BRUSH_CHANNEL_CURVES 3

enum {
  BRUSH_CHANNEL_RADIUS = 1 << 0,
  BRUSH_CHANNEL_STRENGTH = 1 << 1,
  BRUSH_CHANNEL_CLOTH_TYPE = 1 << 2,  // int
  BRUSH_CHANNEL_RADIUS_SCALE = 1 << 3,
  //  BRUSH_CHANNEL_ITERATIONS = 1 << 3,  // int
  //  BRUSH_CHANNEL_BOUNDARY_TYPE = 1 << 4,
  // BRUSH_CHANNEL_AUTOMASKING_TYPE = 1 << 5,
  CHANNEL_CUSTOM = 1 << 20
};

typedef struct BrushChannel {
  int type;
  char name[32];  // for custom types

  float value;
  CurveMapping curves[MAX_BRUSH_CHANNEL_CURVES];
  int flag;
} BrushChannel;

#  define MAX_BRUSH_ENUM_DEF 32

typedef struct BrushEnumDef {
  EnumPropertyItem items[MAX_BRUSH_ENUM_DEF];
} BrushEnumDef;

typedef struct BrushChannelType {
  char name[32];
  int channel;
  float min, max, softmin, softmax;
  int curve_presets[MAX_BRUSH_CHANNEL_CURVES];
  int type, subtype;
  BrushEnumDef enumdef;  // if an enum type
} BrushChannelType;

// curves
enum { BRUSH_CHANNEL_PRESSURE, BRUSH_CHANNEL_XTILT, BRUSH_CHANNEL_YTILT };

enum {
  BRUSH_CHANNEL_FLOAT = 1 << 0,
  BRUSH_CHANNEL_INT = 1 << 1,
  BRUSH_CHANNEL_ENUM = 1 << 0  // subtype
};

/* BrushChannel->flag */
enum {
  /*float only flags*/
  BRUSH_CHANNEL_USE_PRESSURE = 1 << 0,
  BRUSH_CHANNEL_INV_PRESSURE = 1 << 1,
  BRUSH_CHANNEL_USE_TILT = 1 << 2,
};

/*
Brush command lists.

Command lists are built dynamically from
brush flags, pen input settings, etc.

Eventually they will be generated by node
networks.  BrushCommandPreset will be
generated from the node group inputs.
*/

typedef struct BrushCommandPreset {
  char name[64];
  int tool;

  struct {
    char name[32];
    int type;
    float defval;
    BrushChannelType *custom_type;
  } channels[32];
} BrushCommandPreset;

/* clang-format off */
BrushCommandPreset DrawBrush = {
  .name = "Draw",
  .tool = SCULPT_TOOL_DRAW,
  .channels = {
    {.name = "Radius", .type = BRUSH_CHANNEL_RADIUS, .defval = 50.0f},
    {.name = "Strength", .type = BRUSH_CHANNEL_STRENGTH, .defval = 1.0f},
    {.name = "Autosmooth", .type = BRUSH_CHANNEL_STRENGTH, .defval = 0.0f},
    {.name = "Autosmooth Radius Scale", .type = BRUSH_CHANNEL_RADIUS_SCALE, .defval = 1.0f},
    {.name = "Topology Rake", .type = BRUSH_CHANNEL_STRENGTH, .defval = 0.0f},
    {.name = "Topology Rake Radius Scale", .type = BRUSH_CHANNEL_RADIUS_SCALE, .defval = 1.0f},
    {.type = -1}
  }
};

typedef struct BrushCommand {
  int tool;
  BrushChannel params[MAX_BRUSH_COMMAND_PARAMS];
  int totparam;
} BrushCommand;

typedef struct BrushCommandList {
  BrushCommand *commands;
  int totcommand;
} BrushCommandList;

static BrushChannelType brush_builtin_channels[] = {
    {.name = "Radius",
     .channel = BRUSH_CHANNEL_RADIUS,
     .type = BRUSH_CHANNEL_FLOAT,
     .min = 0.001,
     .max = 1024,
     .softmin = 0.001,
     .softmax = 700,
     .curve_presets = {CURVE_PRESET_SMOOTH}},
    {.name = "Strength",
     .channel = BRUSH_CHANNEL_STRENGTH,
     .type = BRUSH_CHANNEL_FLOAT,
     .min = 0.001,
     .max = 1024,
     .softmin = 0.001,
     .softmax = 700,
     .curve_presets = {CURVE_PRESET_SMOOTH}},
    {.name = "Cloth Deform Type",
     .channel = BRUSH_CHANNEL_CLOTH_TYPE,
     .type = BRUSH_CHANNEL_INT,
     .subtype = BRUSH_CHANNEL_ENUM,
     .enumdef =
         {
             {BRUSH_CLOTH_DEFORM_DRAG, "DRAG", 0, "Drag", ""},
             {BRUSH_CLOTH_DEFORM_PUSH, "PUSH", 0, "Push", ""},
             {BRUSH_CLOTH_DEFORM_PINCH_POINT, "PINCH_POINT", 0, "Pinch Point", ""},
             {BRUSH_CLOTH_DEFORM_PINCH_PERPENDICULAR,
              "PINCH_PERPENDICULAR",
              0,
              "Pinch Perpendicular",
              ""},
             {BRUSH_CLOTH_DEFORM_INFLATE, "INFLATE", 0, "Inflate", ""},
             {BRUSH_CLOTH_DEFORM_GRAB, "GRAB", 0, "Grab", ""},
             {BRUSH_CLOTH_DEFORM_EXPAND, "EXPAND", 0, "Expand", ""},
             {BRUSH_CLOTH_DEFORM_SNAKE_HOOK, "SNAKE_HOOK", 0, "Snake Hook", ""},
             {0, NULL, 0, NULL, NULL},
         }},
    {.name = "Radius Scale",
     .channel = BRUSH_CHANNEL_RADIUS,
     .type = BRUSH_CHANNEL_FLOAT,
     .min = 0.001,
     .max = 15.0,
     .softmin = 0.1,
     .softmax = 4.0,
     .curve_presets = {CURVE_PRESET_SMOOTH}}};

enum {
  BRUSH_COMMAND_TOPOLOGY_RAKE = 500,
  BRUSH_COMMAND_DYNTOPO = 501,
  BRUSH_COMMAND_SMOOTH_LAP = 502,
  BRUSH_COMMAND_SMOOTH_SURFACE = 503,
};

/* clang-format on */

enum {
  SCULPT_OP_INT = 1 << 0,
  SCULPT_OP_FLOAT = 1 << 1,
  SCULPT_OP_VEC2 = 1 << 2,
  SCULPT_OP_VEC3 = 1 << 3,
  SCULPT_OP_VEC4 = 1 << 4,
  SCULPT_OP_VEC5 = 1 << 5,
  SCULPT_OP_PTR = 1 << 6
};

typedef union SculptReg {
  float f;
  int i;
  float v[5];
  void *p;
  char ch[32];
  // BrushChannel *ch;
} SculptReg;

typedef struct SculptOpCode {
  int code;
  SculptReg params[16];
} SculptOpCode;

#  define SCULPT_MAXREG 32

typedef struct SculptVM {
  SculptOpCode *opcodes;
  int totopcode;
  SculptReg regs[SCULPT_MAXREG];
};

enum {
  SOP_LOADF = 0,  // reg, float
  SOP_LOADI,
  SOP_LOADF2,   // reg, float, float
  SOP_LOADF3,   // reg, float, float, float
  SOP_LOADF4,   // reg, float, float, float, float
  SOP_LOADPTR,  // reg, ptr

  SOP_PUSH,         // reg
  SOP_POP,          // reg
  SOP_POP_DISCARD,  //
  SOP_MUL,          // dstreg reg reg
  SOP_ADD,          // dstreg reg reg
  SOP_SUB,          // dstreg reg reg
  SOP_DIV,          // dstreg reg reg

  SOP_LOAD_CHANNEL_F,  // reg channel
  SOP_LOAD_CHANNEL_I,  // reg channel
  SOP_TOOL_EXEC,       // tool, ...
};

#  define BRUSH_VALUE_DEFAULT FLT_MAX;

/* clang-format off */
#define REG_RADIUS 10
#define REG_STRENGTH 11
#define REG_AUTOSMOOTH 12
#define REG_TOPORAKE 13

#define OP(op) {op},
#define OP_I_CH(op, i1, ch1) {op, {{.i = i1}, {.ch = ch1}, {.i = -1}}},
#define OP_I(op, i1, i2) {op, {{.i = i1}, {.i = -1}}},
#define OP_I2(op, i1, i2) {op, {{.i = i1}, {.i = i2}, {.i = -1}}},
#define OP_I3(op, i1, i2, i3) {op, {{.i = i1}, {.i = i2}, {.i = i3}, {.i = -1}}},
#define OP_I4(op, i1, i2, i3, i4) {op, {{.i = i1}, {.i = i2}, {.i = i3}, {.i = i4}, {.i = -1}}},

SculptOpCode preset[] = {
  OP_I_CH(SOP_LOAD_CHANNEL_F, REG_RADIUS, "Radius")
  OP_I_CH(SOP_LOAD_CHANNEL_F, REG_STRENGTH, "Strength")
  OP_I3(SOP_TOOL_EXEC, SCULPT_TOOL_DRAW, REG_RADIUS, REG_STRENGTH)

  OP_I_CH(SOP_LOAD_CHANNEL_F, REG_AUTOSMOOTH, "Radius")
  OP_I_CH(SOP_LOAD_CHANNEL_F, REG_AUTOSMOOTH, "Autosmooth Radius Scale")
  OP_I3(SOP_MUL, REG_AUTOSMOOTH, REG_AUTOSMOOTH, REG_RADIUS)
};

typedef struct GraphNode {
  char name[32];
  char id[32];

  struct {
    char src[32];
    char node[32];
    char dst[32];
  } inputs[32];
} GraphNode;

/* node preset solution b:

from brush_builder import Builder;

def build(input, output):
  input.add("Strength", "float", "strength").range(0.0, 3.0)
  input.add("Radius", "float", "radius").range(0.01, 1024.0)
  input.add("Autosmooth", "float", "autosmooth").range(0.0, 4.0)
  input.add("Topology Rake", "float", "topology rake").range(0.0, 4.0)
  input.add("Smooth Radius Scale", "float", "autosmooth_radius_scale").range(0.01, 5.0)
  input.add("Rake Radius Scale", "float", "toporake_radius_scale").range(0.01, 5.0)

  draw = input.make.tool("DRAW")
  draw.radius = input.radius
  draw.strength = input.strength

  smooth = input.make.tool("SMOOTH")
  smooth.radius = input.radius * input.autosmooth_radius_scale
  smooth.strength = input.autosmooth;
  smooth.flow = draw.outflow

  rake = input.make.tool("TOPORAKE")
  rake.radius = input.radius * input.toporake_radius_scale
  rake.strength = input.topology;
  rake.flow = smooth.outflow

  output.out = rake.outflow

preset = Builder(build)

*/



/*
bNodeType sculpt_tool_node = {
  .idname = "SculptTool",
  .ui_name = "SculptTool",
};*/
/* cland-format on */
#endif