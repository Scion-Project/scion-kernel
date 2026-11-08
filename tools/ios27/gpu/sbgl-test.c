/*
 * Project Starbite: first OpenGL ES on the Mali-G72 under iOS. Mesa's
 * Panfrost (tools/ios27/mesa-ios/mk-mesa-ios.sh -> libOSMesa.8.dylib) through
 * the StarbiteMali kernel interface: an OSMesa GLES 3 context, a clear, a
 * shaded triangle, a read-back check, and frames per second for a
 * full-screen-sized draw loop. Results go to stdout (the launchd job's log
 * file) and to the kernel log through StarbiteMali's LOG selector.
 *   tools/ios27/gpu/build-sbgl-test.sh -> out/ios/sbgl-test
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <IOKit/IOKitLib.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/osmesa.h>
#include <GLES3/gl3.h>

static io_connect_t logconn;

static void
klog(const char *fmt, ...)
{
	char text[240];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	printf("%s\n", text);
	fflush(stdout);
	if (logconn != IO_OBJECT_NULL) {
		size_t none = 0;
		IOConnectCallStructMethod(logconn, 0x102, text, strlen(text), NULL, &none);
	}
}

static double
now_ms(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char info[200] = "";
		glGetShaderInfoLog(s, sizeof(info), NULL, info);
		klog("shader compile failed: %s", info);
	}
	return s;
}

/*
 * launchd gives an unlisted daemon a 24 MB jetsam limit (6 MB x sweetmemscale
 * 4) and ignores JetsamProperties in the job: raise our own, as root
 * (MEMORYSTATUS_CMD_SET_JETSAM_TASK_LIMIT, MB).
 */
static void
raise_memory_limit(uint32_t mb)
{
	int (*control)(uint32_t, int32_t, uint32_t, void *, size_t) =
	    (int (*)(uint32_t, int32_t, uint32_t, void *, size_t))dlsym(RTLD_DEFAULT, "memorystatus_control");
	int r = control != NULL ? control(6, getpid(), mb, NULL, 0) : -2;
	int e = errno;
	/* else MEMORYSTATUS_CMD_INCREASE_JETSAM_TASK_LIMIT (22): own pid, no root needed on DEVELOPMENT */
	int r2 = control != NULL && r != 0 ? control(22, getpid(), mb, NULL, 0) : 0;
	printf("memory limit %u MB: set %d (errno %d), increase %d (errno %d), uid %d\n", mb, r, e, r2,
	    r2 ? errno : 0, getuid());
}

int
main(void)
{
	raise_memory_limit(1024);
	io_service_t svc = IO_OBJECT_NULL;
	for (int i = 0; i < 120 && svc == IO_OBJECT_NULL; i++) {
		svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("StarbiteMali"));
		if (svc == IO_OBJECT_NULL) {
			sleep(1);
		}
	}
	if (svc != IO_OBJECT_NULL) {
		IOServiceOpen(svc, mach_task_self(), 0, &logconn);
		IOObjectRelease(svc);
	}

	const int w = 1242, h = 2208;
	uint32_t *buf = calloc((size_t)w * h, 4);
	const int attribs[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 0, OSMESA_STENCIL_BITS, 0,
		OSMESA_PROFILE, OSMESA_ES_PROFILE,
		OSMESA_CONTEXT_MAJOR_VERSION, 3, OSMESA_CONTEXT_MINOR_VERSION, 0, 0,
	};
	double t0 = now_ms();
	OSMesaContext ctx = OSMesaCreateContextAttribs(attribs, NULL);
	if (ctx == NULL || !OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, w, h)) {
		klog("GLES: no context (ctx %p)", (void *)ctx);
		return 1;
	}
	klog("GLES: context in %.0f ms: %s | %s | %s", now_ms() - t0,
	    (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
	    (const char *)glGetString(GL_VERSION));

	klog("GLES: framebuffer status %#x, error %#x", glCheckFramebufferStatus(GL_FRAMEBUFFER), glGetError());

	/* 1. Clear to blue and read back. */
	glViewport(0, 0, w, h);
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	uint32_t px = 0;
	glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &px);
	glFinish();
	klog("GLES: clear read-back %#010x (want 0xff0000ff), user buffer %#010x, error %#x -> %s", px,
	    buf[(size_t)(h / 2) * w + w / 2], glGetError(),
	    px == 0xff0000ffu ? "THE GPU CLEARED THROUGH MESA" : "wrong");

	/* 2. A shaded triangle over the middle. */
	const char *vs =
	    "#version 300 es\n"
	    "in vec2 pos; in vec3 col; out vec3 c;\n"
	    "void main() { c = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";
	const char *fs =
	    "#version 300 es\n"
	    "precision mediump float; in vec3 c; out vec4 o;\n"
	    "void main() { o = vec4(c, 1.0); }\n";
	GLuint prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "pos");
	glBindAttribLocation(prog, 1, "col");
	glLinkProgram(prog);
	GLint linked = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	glUseProgram(prog);
	static const float verts[] = {
		-0.8f, -0.6f, 1, 0, 0,
		0.8f, -0.6f, 0, 1, 0,
		0.0f, 0.8f, 0, 0, 1,
	};
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 20, (void *)0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 20, (void *)8);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	uint32_t mid = 0, corner = 0;
	glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &mid);
	glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &corner);
	int r = mid & 0xff, g = (mid >> 8) & 0xff, b = (mid >> 16) & 0xff;
	klog("GLES: triangle (link %d, error %#x): middle %#010x, corner %#010x -> %s", linked,
	    glGetError(), mid, corner,
	    (r > 40 && g > 40 && b > 40 && corner == 0xff333333u) ? "THE GPU DREW A TRIANGLE THROUGH MESA" : "wrong");

	/* 3. Frames per second like a CoreAnimation frame: into an off-screen
	 *    1242 x 2208 texture (no read-back to the OSMesa buffer), a clear
	 *    and 5 full-screen textured, blended quads ("layers"). */
	const char *tvs =
	    "#version 300 es\n"
	    "in vec2 pos; out vec2 uv;\n"
	    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
	const char *tfs =
	    "#version 300 es\n"
	    "precision mediump float; uniform sampler2D t; uniform float a; in vec2 uv; out vec4 o;\n"
	    "void main() { o = vec4(texture(t, uv).rgb, a); }\n";
	GLuint tprog = glCreateProgram();
	glAttachShader(tprog, shader(GL_VERTEX_SHADER, tvs));
	glAttachShader(tprog, shader(GL_FRAGMENT_SHADER, tfs));
	glBindAttribLocation(tprog, 0, "pos");
	glLinkProgram(tprog);
	glUseProgram(tprog);
	GLint ua = glGetUniformLocation(tprog, "a");
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLuint qvbo;
	glGenBuffers(1, &qvbo);
	glBindBuffer(GL_ARRAY_BUFFER, qvbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
	glEnableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	for (int i = 0; i < w * h; i++) {
		buf[i] = 0xff000000u | (uint32_t)(i * 2654435761u >> 8);
	}
	GLuint tex[2], fbo;
	glGenTextures(2, tex);
	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, tex[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, i == 0 ? buf : NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
	klog("GLES: off-screen target status %#x, error %#x", glCheckFramebufferStatus(GL_FRAMEBUFFER),
	    glGetError());
	glBindTexture(GL_TEXTURE_2D, tex[0]);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	t0 = now_ms();
	int frames = 0;
	double first = 0;
	while (now_ms() - t0 < 4000.0) {
		glClearColor((frames & 1) ? 0.1f : 0.3f, 0.2f, 0.2f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		for (int i = 0; i < 5; i++) {
			glUniform1f(ua, 0.3f + 0.1f * i);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glFinish();
		if (frames++ == 0) {
			first = now_ms() - t0;
		}
	}
	double secs = (now_ms() - t0) / 1000.0;
	klog("GLES: %d frames of clear + 5 full-screen textured blended layers at %dx%d in %.1f s = %.1f fps "
	    "(first frame %.0f ms), error %#x", frames, w, h, secs, frames / secs, first, glGetError());

	OSMesaDestroyContext(ctx);
	free(buf);
	return 0;
}
