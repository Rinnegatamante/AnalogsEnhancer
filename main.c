#include <vitasdkkern.h>
#include <taihen.h>
#include <libk/string.h>
#include <libk/stdio.h>
#include <libk/stdlib.h>
#include <math.h>

#define HOOKS_NUM   3  // Hooked functions num

static uint8_t current_hook = 0;
static SceUID hooks[HOOKS_NUM];
static tai_hook_ref_t refs[HOOKS_NUM];

static uint32_t deadzoneLeft, deadzoneRight;
static char buffer[32];
static char rescaleLeft, rescaleRight;

static void (*patchFuncLeft)(uint8_t *x, uint8_t *y, int dead);
static void (*patchFuncRight)(uint8_t *x, uint8_t *y, int dead);

// Courtesy of rsn8887
void rescaleAnalogs(uint8_t *x, uint8_t *y, int dead) {
	//radial and scaled deadzone
	//http://www.third-helix.com/2013/04/12/doing-thumbstick-dead-zones-right.html
	//input and output values go from 0...255;
	
	if (dead == 0) return;
    if (dead > 126) {
        *x = 127;
        *y = 127;
        return;
    } 

	float analogX = (float) *x - 127.0f;
    float analogY = (float) *y - 127.0f;
    float deadZone = (float) dead;
    float magnitude = sqrt(analogX * analogX + analogY * analogY);
    if (magnitude >= deadZone){
        //adjust maximum magnitude
        float absAnalogX = fabs(analogX);
        float absAnalogY = fabs(analogY);
        float maximum;
        if (absAnalogX > absAnalogY)
            maximum = sqrt(127.0f * 127.0f + ((127.0f * analogY) / absAnalogX) * ((127.0f * analogY) / absAnalogX));
        else
            maximum = sqrt(127.0f * 127.0f + ((127.0f * analogX) / absAnalogY) * ((127.0f * analogX) / absAnalogY));
 
        if (maximum > 1.25f * 127.0f) maximum = 1.25f * 127.0f;
        if (maximum < magnitude) maximum = magnitude;
       
        // find scaled axis values with magnitudes between zero and maximum
        float scalingFactor = maximum / magnitude * (magnitude - deadZone) / (maximum - deadZone);     
        analogX = (analogX * scalingFactor);
        analogY = (analogY * scalingFactor);
 
        // clamp to ensure results will always lie between 0 and 255
        float clampingFactor = 1.0f;
        absAnalogX = fabs(analogX);
        absAnalogY = fabs(analogY);
        if (absAnalogX > 127.0f || absAnalogY > 127.0f){
            if (absAnalogX > absAnalogY)
                clampingFactor = 127.0f / absAnalogX;
            else
                clampingFactor = 127.0f / absAnalogY;
        }
 
        *x = (uint8_t) ((clampingFactor * analogX) + 127.0f);
        *y = (uint8_t) ((clampingFactor * analogY) + 127.0f);
    }else{
        *x = 127;
        *y = 127;
    }
}

void deadzoneAnalogs(uint8_t *x, uint8_t *y, int dead) {
	
	if (dead == 0) return;
    if (dead > 126) {
        *x = 127;
        *y = 127;
        return;
    } 
	
	float analogX = (float) *x - 127.0f;
	float analogY = (float) *y - 127.0f;
	float deadZone = (float) dead;
	float magnitude = sqrt(analogX * analogX + analogY * analogY);
	if (magnitude < deadZone){
		*x = 127;
		*y = 127;
	}
}

void patchData(uint8_t *data) {
	patchFuncLeft(&data[12], &data[13], deadzoneLeft);
	patchFuncRight(&data[14], &data[15], deadzoneRight);
}

void loadConfig(void) {

	// Just in case the folder doesn't exist
	ksceIoMkdir("ux0:data/AnalogsEnhancer", 0777); 
	
	// Loading generic config file
	SceUID fd = ksceIoOpen("ux0:/data/AnalogsEnhancer/config.txt", SCE_O_RDONLY, 0777);
	if (fd >= 0){
		ksceIoRead(fd, buffer, 32);
		ksceIoClose(fd);
	}else sprintf(buffer, "0;n");
	sscanf(buffer, "left=%lu,%c;right=%lu,%c", &deadzoneLeft, &rescaleLeft, &deadzoneRight, &rescaleRight);
	
	if (rescaleLeft == 'y') patchFuncLeft = rescaleAnalogs;
	else patchFuncLeft = deadzoneAnalogs;
	if (rescaleRight == 'y') patchFuncRight = rescaleAnalogs;
	else patchFuncRight = deadzoneAnalogs;
	
}

// Simplified generic hooking functions
void hookFunctionExport(uint32_t nid, const void *func, const char *module) {
	hooks[current_hook] = taiHookFunctionExportForKernel(KERNEL_PID, &refs[current_hook], module, TAI_ANY_LIBRARY, nid, func);
	current_hook++;
}

void hookOffset(uint32_t offs, const void *func, SceUID id) {
	hooks[current_hook] = taiHookFunctionOffsetForKernel(KERNEL_PID, &refs[current_hook], id, 0, offs, 1, func);
}

int ksceCtrlSetSamplingMode_patched(SceCtrlPadInputMode mode) {
	if (mode == SCE_CTRL_MODE_ANALOG) mode = SCE_CTRL_MODE_ANALOG_WIDE;
	return TAI_CONTINUE(int, refs[0], mode);
}

int ksceCtrlPeekBufferPositive_patched(int port, SceCtrlData *ctrl, int count) {
	int ret = TAI_CONTINUE(int, refs[1], port, ctrl, count);
	patchData((uint8_t*)ctrl);
	return ret;
}

int ksceCtrlReadBufferPositive_patched(int port, SceCtrlData *ctrl, int count) {
	int ret = TAI_CONTINUE(int, refs[2], port, ctrl, count);
	patchData((uint8_t*)ctrl);
	return ret;
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
	
	// Setup stuffs
	loadConfig();
	
	// Hooking functions
	hookFunctionExport(0x80F5E418, ksceCtrlSetSamplingMode_patched, "SceCtrl");
	hookFunctionExport(0xEA1D3A34, ksceCtrlPeekBufferPositive_patched, "SceCtrl");
	hookFunctionExport(0x9B96A1AA, ksceCtrlReadBufferPositive_patched, "SceCtrl");
	
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {

	// Freeing hooks
	while (current_hook-- > 0){
		taiHookReleaseForKernel(hooks[current_hook], refs[current_hook]);
	}
		
	return SCE_KERNEL_STOP_SUCCESS;
	
}