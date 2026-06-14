/************************************************************************************

Filename	:	VrInputRight.c 
Content		:	Handles common controller input functionality
Created		:	September 2019
Authors		:	Simon Brown

*************************************************************************************/

#include "VrInput.h"

#include <src/qcommon/qcommon.h>

//keys.h
void Sys_QueEvent( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr );
void handleTrackedControllerButton(ovrInputStateTrackedRemote * trackedRemoteState, ovrInputStateTrackedRemote * prevTrackedRemoteState, uint32_t button, int key)
{
    if ((trackedRemoteState->Buttons & button) != (prevTrackedRemoteState->Buttons & button))
    {
        Sys_QueEvent( 0, SE_KEY, key, (trackedRemoteState->Buttons & button) != 0, 0, NULL );
//        Key_Event(key, (trackedRemoteState->Buttons & button) != 0, global_time);
    }
}

void rotateAboutOrigin(float x, float y, float rotation, vec2_t out)
{
    out[0] = cosf(DEG2RAD(-rotation)) * x  +  sinf(DEG2RAD(-rotation)) * y;
    out[1] = cosf(DEG2RAD(-rotation)) * y  -  sinf(DEG2RAD(-rotation)) * x;
}

float length(float x, float y)
{
    return sqrtf(powf(x, 2.0f) + powf(y, 2.0f));
}

#define NLF_DEADZONE 0.1
#define NLF_POWER 2.2

float nonLinearFilter(float in)
{
    float val = 0.0f;
    if (in > NLF_DEADZONE)
    {
        val = in;
        val -= NLF_DEADZONE;
        val /= (1.0f - NLF_DEADZONE);
        val = powf(val, NLF_POWER);
    }
    else if (in < -NLF_DEADZONE)
    {
        val = in;
        val += NLF_DEADZONE;
        val /= (1.0f - NLF_DEADZONE);
        val = -powf(fabsf(val), NLF_POWER);
    }

    return val;
}

void sendButtonActionSimple(const char* action)
{
    char command[256];
    snprintf( command, sizeof( command ), "%s\n", action );
    Cbuf_AddText( command );
}

qboolean between(float min, float val, float max)
{
    return (min < val) && (val < max);
}

void sendButtonAction(const char* action, long buttonDown)
{
    char command[256];
    snprintf( command, sizeof( command ), "%s\n", action );
    if (!buttonDown)
    {
        command[0] = '-';
    }
    Cbuf_AddText( command );
}

void acquireTrackedRemotesData(double displayTime) {
    (void)displayTime;
    TBXR_UpdateControllers();
}


//YAW:  Left increase, Right decrease
void updateScopeAngles()
{
    //Bit of a hack, but use weapon orientation / position for view when scope is engaged
    static vec3_t currentScopeAngles;
    static vec3_t lastScopeAngles;
    if (vr.scopeengaged)
    {
        //Clear weapon offset
        VectorSet(vr.calculated_weaponoffset, 0, 0, 0);

        VectorSet(currentScopeAngles, vr.weaponangles[PITCH], vr.weaponangles[YAW], vr.hmdorientation[ROLL]);

        //Set "view" Angles
        VectorCopy(currentScopeAngles, vr.hmdorientation);

        //Orientation
        VectorSubtract(lastScopeAngles, currentScopeAngles, vr.hmdorientation_delta);

        //Keep this for our records
        VectorCopy(currentScopeAngles, lastScopeAngles);
    } else {
        VectorSet(currentScopeAngles, vr.weaponangles[PITCH], vr.weaponangles[YAW], vr.hmdorientation[ROLL]);
        VectorCopy(currentScopeAngles, lastScopeAngles);
    }
}

void PortableMouseAbs(float x,float y);
inline float clamp(float _min, float _val, float _max)
{
    return max(min(_val, _max), _min);
}

void interactWithTouchScreen(float menuYaw, vec3_t controllerAngles) {
    float cursorX = -sinf(DEG2RAD(controllerAngles[YAW] - menuYaw)) + 0.5f;
    float cursorY = (float)((controllerAngles[PITCH] - 15) / 90.0) + 0.5f;

    PortableMouseAbs(cursorX, cursorY);
}
