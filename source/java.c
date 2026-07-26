#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>
#include <falso_jni/FalsoJNI_ImplBridge.h>
#include <psp2/kernel/clib.h>
#include <psp2/audioout.h>

// Audio. The game's sound driver builds an android.media.AudioTrack over JNI and
// pushes 16-bit PCM into it with write(), so AudioTrack is faked here and the PCM
// forwarded to sceAudioOut. sceAudioOutOutput blocks until the buffer is consumed,
// which paces the game's audio thread for free.
#define AT_GRAIN 1024                 // frames per sceAudioOut call
static int   at_port = -1;
static int   at_freq = 44100;
static int   at_stereo = 1;
static short at_accum[AT_GRAIN * 2];  // one stereo grain
static int   at_accum_frames = 0;

static void at_ensure_port(void) {
	if (at_port >= 0) return;
	// sceAudioOut only takes these rates, so snap to the closest one.
	int ok[] = {48000,44100,32000,24000,22050,16000,12000,11025,8000};
	int best = 44100, bd = 1<<30;
	for (int i = 0; i < 9; i++) { int d = at_freq>ok[i]?at_freq-ok[i]:ok[i]-at_freq; if (d<bd){bd=d;best=ok[i];} }
	at_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AT_GRAIN, best,
	                              at_stereo ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO);
	sceClibPrintf("9MM-AUD: sceAudioOutOpenPort(freq=%d->%d stereo=%d) -> 0x%x\n",
	              at_freq, best, at_stereo, at_port);
	if (at_port >= 0) sceAudioOutSetConfig(at_port, -1, -1, -1);
}

// Accumule des frames PCM16 et sort des grains complets (bloquant).
static void at_output_pcm16(const short *pcm, int frames) {
	at_ensure_port();
	if (at_port < 0) return;
	int step = at_stereo ? 2 : 1;
	while (frames > 0) {
		int take = AT_GRAIN - at_accum_frames;
		if (take > frames) take = frames;
		sceClibMemcpy(&at_accum[at_accum_frames * step], pcm, take * step * sizeof(short));
		at_accum_frames += take;
		pcm += take * step;
		frames -= take;
		if (at_accum_frames == AT_GRAIN) {
			sceAudioOutOutput(at_port, at_accum);  // bloque
			at_accum_frames = 0;
		}
	}
}

jint j_at_getMinBufferSize(jmethodID id, va_list args) {
	int sr = va_arg(args, int), ch = va_arg(args, int), fmt = va_arg(args, int);
	sceClibPrintf("9MM-AUD: getMinBufferSize(sr=%d ch=%d fmt=%d)\n", sr, ch, fmt);
	return AT_GRAIN * 4; // bytes, 16-bit stereo
}
jobject j_at_init(jmethodID id, va_list args) {
	int stream = va_arg(args, int);
	at_freq = va_arg(args, int);
	int ch = va_arg(args, int), fmt = va_arg(args, int);
	int buf = va_arg(args, int), mode = va_arg(args, int);
	at_stereo = (ch == 12 || ch == 3) ? 1 : 0; // CHANNEL_OUT_STEREO
	sceClibPrintf("9MM-AUD: AudioTrack(stream=%d freq=%d ch=%d fmt=%d buf=%d mode=%d) stereo=%d\n",
	              stream, at_freq, ch, fmt, buf, mode, at_stereo);
	return (jobject)0xA0D10001; // objet factice non-NULL
}
jint j_at_write(jmethodID id, va_list args) {
	jbyteArray arr = va_arg(args, jbyteArray);
	int off = va_arg(args, int), size = va_arg(args, int);
	JavaDynArray *jda = (JavaDynArray *)arr;
	if (!jda || !jda->array || size <= 0) return 0;
	const short *pcm = (const short *)((const uint8_t *)jda->array + off);
	at_output_pcm16(pcm, size / (2 * (at_stereo ? 2 : 1)));
	return size;
}
void j_at_play(jmethodID id, va_list args)  { }
void j_at_pause(jmethodID id, va_list args) { }
void j_at_stop(jmethodID id, va_list args)  { at_accum_frames = 0; }
void j_at_release(jmethodID id, va_list args) {
	if (at_port >= 0) { sceAudioOutReleasePort(at_port); at_port = -1; }
}

/*
 * JNI Methods
*/

// NewStringUTF is a plain function in FalsoJNI.c, only exposed through the JNIEnv
// method pointers, so declare it here.
extern jstring NewStringUTF(JNIEnv* env, const char* bytes);

// Pull the char* out of a FalsoJNI jstring (utf16 -> utf8), NULL if there isn't one.
static const char* jstr_cstr(jstring s) {
    JavaString* js = (JavaString*)s;
    if (!js || !js->utf16) return NULL;
    if (jstr_utf16_to_utf8(js) != JNI_TRUE || !js->utf8) return NULL;
    return (const char*)js->utf8->array;
}

// GLResLoader resource callbacks. The game asks for resources by name here; the
// data itself comes off disk through the io remap instead, so these only need to
// answer without upsetting it.
jint j_getResourceLength(jmethodID id, va_list args) {
    const char* n = jstr_cstr(va_arg(args, jstring));
    sceClibPrintf("9MM-RES: getResourceLength(\"%s\")\n", n ? n : "(null)");
    return 0;
}
jint j_getResourceLengthSoundRaw(jmethodID id, va_list args) {
    int idx = va_arg(args, int);
    sceClibPrintf("9MM-RES: getResourceLengthSoundRaw(%d)\n", idx);
    return 0;
}
jobject j_getResourceFull(jmethodID id, va_list args) {
    const char* n = jstr_cstr(va_arg(args, jstring));
    sceClibPrintf("9MM-RES: getResourceFull(\"%s\")\n", n ? n : "(null)");
    return NULL;
}
jobject j_getResourceBytes(jmethodID id, va_list args) {
    const char* n = jstr_cstr(va_arg(args, jstring));
    sceClibPrintf("9MM-RES: getResourceBytes(\"%s\")\n", n ? n : "(null)");
    return NULL;
}
jobject j_getSoundRaw(jmethodID id, va_list args) {
    int idx = va_arg(args, int);
    sceClibPrintf("9MM-RES: getSoundRaw(%d)\n", idx);
    return NULL;
}

// Generic zero-returning stub for the device detection methods.
// These matter more than they look. The native side does `id = GetStaticMethodID(
// ...); if (id) { v = call(); ... }`, so leaving one unregistered skips the whole
// block, and one of those blocks is the GPU detection that sets the .tga -> .pvr
// remap flag. Without it every texture comes out magenta. Zero is what GLGame
// returns on Android anyway.
jint j_ret0_int(jmethodID id, va_list args) { return 0; }

NameToMethodID nameToMethodId[] = {
		{ 100, "getResourceFull",           METHOD_TYPE_OBJECT },
		{ 101, "getResourceBytes",          METHOD_TYPE_OBJECT },
		{ 102, "getResourceLength",         METHOD_TYPE_INT },
		{ 103, "getSoundRaw",               METHOD_TYPE_OBJECT },
		{ 104, "getResourceLengthSoundRaw", METHOD_TYPE_INT },
		{ 200, "DetectDeviceGPU",           METHOD_TYPE_INT },
		{ 201, "getBuildType",              METHOD_TYPE_INT },
		{ 202, "detectLanguage",            METHOD_TYPE_INT },
		{ 203, "HasGyroscope",              METHOD_TYPE_INT },
		{ 204, "Is3GEnabled",               METHOD_TYPE_INT },
		{ 205, "IsWifiEnabled",             METHOD_TYPE_INT },
		{ 206, "isExternalMusicActive",     METHOD_TYPE_INT },
		{ 207, "isNotUseSoundEngine",       METHOD_TYPE_INT },
		{ 208, "IsAcerA500",                METHOD_TYPE_INT },
		{ 209, "IsSGHi897",                 METHOD_TYPE_INT },
		{ 210, "IsSCHi510",                 METHOD_TYPE_INT },
		{ 211, "IsUseExternalCard1",        METHOD_TYPE_INT },
		{ 212, "isHTC",                     METHOD_TYPE_INT },
		{ 213, "getWifiIP",                 METHOD_TYPE_INT },
		{ 214, "isKeyboardVisible",         METHOD_TYPE_INT },
		// --- AudioTrack (le driver VOX pousse le PCM ici) ---
		{ 300, "android/media/AudioTrack/<init>", METHOD_TYPE_OBJECT },
		{ 301, "getMinBufferSize",          METHOD_TYPE_INT },
		{ 302, "write",                     METHOD_TYPE_INT },
		{ 303, "play",                      METHOD_TYPE_VOID },
		{ 304, "pause",                     METHOD_TYPE_VOID },
		{ 305, "stop",                      METHOD_TYPE_VOID },
		{ 306, "release",                   METHOD_TYPE_VOID },
};

MethodsBoolean methodsBoolean[] = {};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};
MethodsInt methodsInt[] = {
		{ 102, j_getResourceLength },
		{ 104, j_getResourceLengthSoundRaw },
		{ 200, j_ret0_int }, { 201, j_ret0_int }, { 202, j_ret0_int },
		{ 203, j_ret0_int }, { 204, j_ret0_int }, { 205, j_ret0_int },
		{ 206, j_ret0_int }, { 207, j_ret0_int }, { 208, j_ret0_int },
		{ 209, j_ret0_int }, { 210, j_ret0_int }, { 211, j_ret0_int },
		{ 212, j_ret0_int }, { 213, j_ret0_int }, { 214, j_ret0_int },
		{ 301, j_at_getMinBufferSize }, { 302, j_at_write },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
		{ 100, j_getResourceFull },
		{ 101, j_getResourceBytes },
		{ 103, j_getSoundRaw },
		{ 300, j_at_init },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
		{ 303, j_at_play }, { 304, j_at_pause },
		{ 305, j_at_stop }, { 306, j_at_release },
};

/*
 * JNI Fields
*/

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// System-wide constant that's often used to determine Android version
// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
// Possible values: https://developer.android.com/reference/android/os/Build.VERSION_CODES
const int SDK_INT = 19; // Android 4.4 / KitKat

// android.os.Build fields, read during startup on the DRM path
// (nativeGDrmGetDevID). The values don't matter, but they can't be null: the game
// calls strlen() on them.
char BUILD_MANUFACTURER[] = "Sony";
char BUILD_MODEL[]        = "PCH-2000";
char BUILD_DEVICE[]       = "PSVita";
char BUILD_PRODUCT[]      = "PSVita";

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
		{ 1, "SDK_INT", FIELD_TYPE_INT },
		{ 2, "MANUFACTURER", FIELD_TYPE_OBJECT },
		{ 3, "MODEL",        FIELD_TYPE_OBJECT },
		{ 4, "DEVICE",       FIELD_TYPE_OBJECT },
		{ 5, "PRODUCT",      FIELD_TYPE_OBJECT },
};

// Swap the raw Build.* values for real jstrings before the game starts. Left as
// plain char*, GetStaticObjectField hands the game something it dereferences as a
// JavaString, and GetStringUTFChars aborts on the garbage it finds.
void java_init(void) {
	for (int i = 0; i < (int)(fieldsObject_size() / sizeof(FieldsObject)); i++) {
		switch (fieldsObject[i].id) {
			case 2: fieldsObject[i].value = NewStringUTF(NULL, BUILD_MANUFACTURER); break;
			case 3: fieldsObject[i].value = NewStringUTF(NULL, BUILD_MODEL); break;
			case 4: fieldsObject[i].value = NewStringUTF(NULL, BUILD_DEVICE); break;
			case 5: fieldsObject[i].value = NewStringUTF(NULL, BUILD_PRODUCT); break;
		}
	}
}

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
		{ 2, BUILD_MANUFACTURER },
		{ 3, BUILD_MODEL },
		{ 4, BUILD_DEVICE },
		{ 5, BUILD_PRODUCT },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
