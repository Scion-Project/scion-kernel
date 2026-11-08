/*
 * What src/eagl.c asks of the OpenGL ES library behind it besides the gl* functions: see
 * src/gl_osmesa.c, the one implementation there is.
 */
#ifndef HAKIOS_GL_H
#define HAKIOS_GL_H

struct hakios_gl;

struct hakios_gl *hakios_gl_create(int version);              /* a context of OpenGL ES 2 or 3; NULL: none to be had */
int hakios_gl_make_current(struct hakios_gl *gl);             /* ... the calling thread's (NULL: none); 0 on failure */
void hakios_gl_destroy(struct hakios_gl *gl);
const char *hakios_gl_library(void);                           /* for the log */

/*
 * The texture bound to GL_TEXTURE_2D made of an IOSurface's own memory: what is drawn into the
 * texture is in the surface, what is in the surface is what the texture shows, and nobody copies.
 * For a library whose driver can do that (a GPU's) and a surface it can do it with. 0: not
 * done; the texture is to be filled and read back with OpenGL ES calls.
 */
int hakios_gl_surface_texture(struct hakios_gl *gl, uint32_t surface);

/*
 * CoreAnimation draws a layer's pixels with one of ninety "functions" (a fragment shader each); these
 * are the names its Metal renderer has for them, four characters a function, in their order.
 * Starbite: iPadOS 18.7's QuartzCore (22H384), 106 of them (iOS 16.5 had 90; the numbers moved), from
 * CA::OGL::MetalContext::TextureFunction_codes (0x1e6994d48), indexed by CA::OGL::TextureFunction.
 */
#ifndef HAKIOS_IOS27
#define HAKIOS_IOS27 1
#endif
#if HAKIOS_IOS27
#undef HAKIOS_IOS26
#define HAKIOS_IOS26 1
#endif
#ifndef HAKIOS_IOS26
#define HAKIOS_IOS26 1
#endif
#if HAKIOS_IOS27
/* iOS 27.0.1's QuartzCore (24A446): 136 of them (r2e new at 50; 101 read as srl by position). */
#define HAKIOS_CA_FUNCTIONS \
	"clr img ida cim cid mua m1a lrp lrc lra add spl s1a cma pnc bds bco bim c33 c3b c3s c4b c4p c3a c3x c4x bia ds4 " \
	"ds6 ds8 ups db2 db4 nb7 n11 n15 n19 n23 n27 vrd vrc vrb cab cam dpm sb3 rbw axl rad rac r2e con bvc vcm vdk vlt " \
	"map mpc crv dfv dff dfg dcg dfs dfp dgh dgd kfh dkf sgr gba gbb gbc gbd gbe gbf gbg gbh gsa gsb gsc gsd gse gsf " \
	"gsg gsh gfg gfs hue clm hom rim srm bri brj dgf dgg bro rio opa met srl ega egm sdr ast alt p3c edc ced edr s2l " \
	"l2s lub aup alm stl sta sai pc0 pc1 prc ps0 pcr pbl peo bib irb asg a75 a50 atc ipc apl 3dl cst "
#define HAKIOS_CA_FUNCTION_COUNT 136
#elif HAKIOS_IOS26
/* iOS 26.6.2's QuartzCore (23G90): 121 of them (build/ca-gpu/sbca-ios26-offsets.md). */
#define HAKIOS_CA_FUNCTIONS \
	"clr img ida cim cid mua m1a lrp lrc lra add spl s1a cma pnc bds bco bim c33 c3b c3s c4b c4p c3a c3x c4x bia ds4 " \
	"ds6 ds8 ups db2 db4 nb7 n11 n15 n19 n23 n27 vrd vrc vrb cab cam dpm sb3 rbw axl rad rac con bvc vcm vdk vlt map " \
	"mpc crv dfv dff dfg dfc dfs dfp dgh dgd kfh sgr gbh ghs ghn ghz gfg gfs hue clm hom rim srm bri brj dgf dgg bro " \
	"rio opa met srl ega egm sdr ast alt p3c edc ced edr s2l l2s lub aup alm stl sta pc0 pc1 prc ps0 pcr pbl peo bib " \
	"irb asg a75 a50 atc ipc apl 3dl cst "
#define HAKIOS_CA_FUNCTION_COUNT 121
#else
#define HAKIOS_CA_FUNCTIONS \
	"clr img ida cim cid mua m1a lrp lrc lra add spl s1a cma pnc bds bco bim c33 c3b c3s c4b c4p c3a c3x c4x bia ds4 " \
	"ds6 ds8 ups db2 db4 nb7 n11 n15 n19 n23 n27 vrd vrb cab sb3 rbw axl rad rac con vcm vdk vlt map mpc crv hue clm " \
	"hom rim srm bri brj bro rio opa met srl ega egm sdr ast alt p3c edc ced edr hrm s2l l2s lut ovr ovl lub aup alm " \
	"stl sta pc0 pc1 prc psl ps0 psm psr pcr pbl peo bib irb asg a75 a50 atc ipc apl 3dl cst "
#define HAKIOS_CA_FUNCTION_COUNT 106
#endif

#endif
