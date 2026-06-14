/*
===========================================================================

Return to Castle Wolfenstein single player GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company. 

This file is part of the Return to Castle Wolfenstein single player GPL Source Code (RTCW SP Source Code).  

RTCW SP Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTCW SP Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTCW SP Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the RTCW SP Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the RTCW SP Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "tr_local.h"

#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif

volatile renderCommandList_t    *renderCommandList;

volatile qboolean renderThreadActive;

qboolean VR_GetFovTangentsForEye(int eye, float *tanLeft, float *tanRight, float *tanUp, float *tanDown);
float VR_GetEyeStereoSeparation(int eye);

typedef union {
	void    *align;
	byte    cmds[MAX_RENDER_COMMANDS + sizeof( int ) + sizeof( void * )];
} vrStereoReplayBuffer_t;

static vrStereoReplayBuffer_t vrStereoReplayCommands;
static vrStereoReplayBuffer_t vrStereoReplayScratch;
static int vrStereoReplayCommandBytes;
static qboolean vrStereoReplayActive;
static qboolean vrStereoReplayLoggedInvalid;

typedef struct {
	int drawBuffers;
	int drawSurfs;
	int stretchPics;
	int flushes;
	int swaps;
} vrStereoReplayStats_t;

static qboolean R_VR_InspectStereoReplayCommands( const byte *cmds, vrStereoReplayStats_t *stats ) {
	const void *curCmd = cmds;

	memset( stats, 0, sizeof( *stats ) );

	while ( 1 ) {
		switch ( *(const int *)curCmd ) {
		case RC_SET_COLOR:
			curCmd = (const void *)( (const setColorCommand_t *)curCmd + 1 );
			break;
		case RC_STRETCH_PIC:
		case RC_STRETCH_PIC_GRADIENT:
			stats->stretchPics++;
			curCmd = (const void *)( (const stretchPicCommand_t *)curCmd + 1 );
			break;
		case RC_DRAW_SURFS:
			stats->drawSurfs++;
			curCmd = (const void *)( (const drawSurfsCommand_t *)curCmd + 1 );
			break;
		case RC_DRAW_BUFFER:
			stats->drawBuffers++;
			curCmd = (const void *)( (const drawBufferCommand_t *)curCmd + 1 );
			break;
		case RC_FLUSH:
			stats->flushes++;
			curCmd = (const void *)( (const swapBuffersCommand_t *)curCmd + 1 );
			break;
		case RC_SWAP_BUFFERS:
			stats->swaps++;
			curCmd = (const void *)( (const swapBuffersCommand_t *)curCmd + 1 );
			break;
		case RC_END_OF_LIST:
			return qtrue;
		default:
			return qfalse;
		}
	}
}

static void R_VR_GetHudReplayOffset( stereoFrame_t stereoFrame, float *x, float *y ) {
	int eye;
	float tanLeft, tanRight, tanUp, tanDown;
	float angleLeft, angleRight, angleUp, angleDown;
	float offX640, offY480;
	float unionLeft, unionRight, unionUp, unionDown;
	float fovXRad;
	float depth;
	float parallax;
	cvar_t *hudDepth;

	*x = 0.0f;
	*y = 0.0f;

	if ( stereoFrame != STEREO_LEFT && stereoFrame != STEREO_RIGHT ) {
		return;
	}

	eye = ( stereoFrame == STEREO_LEFT ) ? 0 : 1;
	if ( !VR_GetFovTangentsForEye( eye, &tanLeft, &tanRight, &tanUp, &tanDown ) ) {
		return;
	}

	angleLeft = atan( tanLeft );
	angleRight = atan( tanRight );
	angleUp = atan( tanUp );
	angleDown = atan( tanDown );

	offX640 = -0.5f * ( angleLeft + angleRight ) * 640.0f;
	offY480 = -0.5f * ( angleUp + angleDown ) * 480.0f;

	if ( VR_GetFovTangentsForEye( -1, &unionLeft, &unionRight, &unionUp, &unionDown ) ) {
		fovXRad = fabs( atan( unionLeft ) ) + fabs( atan( unionRight ) );
	} else {
		fovXRad = fabs( angleLeft ) + fabs( angleRight );
	}
	if ( fovXRad < 1.0f * (float)M_PI / 180.0f ) {
		fovXRad = 90.0f * (float)M_PI / 180.0f;
	}

	hudDepth = ri.Cvar_Get( "cg_hudDepth", "2.0", 0 );
	depth = hudDepth ? hudDepth->value : 2.0f;
	if ( depth < 0.5f ) {
		depth = 0.5f;
	}
	parallax = ( atan2( 0.032f, depth ) / ( fovXRad * 0.5f ) ) * 320.0f;

	if ( eye == 1 ) {
		offX640 -= 2.0f * parallax;
	}

	*x = offX640 * ( (float)glConfig.vidWidth / 640.0f );
	*y = -offY480 * ( (float)glConfig.vidHeight / 480.0f );
}

static void R_VR_PatchStretchPicCommand( stretchPicCommand_t *cmd, stereoFrame_t stereoFrame ) {
	float x, y;

	if ( cmd->x <= 1.0f && cmd->y <= 1.0f &&
		 cmd->w >= glConfig.vidWidth - 2.0f &&
		 cmd->h >= glConfig.vidHeight - 2.0f ) {
		return;
	}

	R_VR_GetHudReplayOffset( stereoFrame, &x, &y );
	cmd->x += x;
	cmd->y += y;
}

static void R_VR_PatchDrawSurfsCommand( drawSurfsCommand_t *cmd, stereoFrame_t stereoFrame ) {
	int eye;
	trRefdef_t savedRefdef;
	viewParms_t savedViewParms;

	if ( stereoFrame != STEREO_LEFT && stereoFrame != STEREO_RIGHT ) {
		return;
	}

	eye = ( stereoFrame == STEREO_LEFT ) ? 0 : 1;
	cmd->refdef.stereoView = stereoFrame;
	cmd->viewParms.stereoFrame = stereoFrame;

	if ( !( cmd->refdef.rdflags & RDF_NOWORLDMODEL ) &&
		 !( cmd->refdef.rdflags & RDF_SKYBOXPORTAL ) ) {
		float sep = VR_GetEyeStereoSeparation( eye ) * cmd->refdef.worldscale;
		VectorMA( cmd->refdef.vieworg, sep, cmd->refdef.viewaxis[1], cmd->refdef.vieworg );
		VectorCopy( cmd->refdef.vieworg, cmd->viewParms.or.origin );
		VectorCopy( cmd->refdef.vieworg, cmd->viewParms.pvsOrigin );
	}

	savedRefdef = tr.refdef;
	savedViewParms = tr.viewParms;
	tr.refdef = cmd->refdef;
	tr.viewParms = cmd->viewParms;
	R_SetupProjection();
	R_SetupFrustum();
	cmd->viewParms = tr.viewParms;
	tr.refdef = savedRefdef;
	tr.viewParms = savedViewParms;
}

static void R_VR_PatchStereoReplayCommands( byte *cmds, stereoFrame_t stereoFrame ) {
	void *curCmd = cmds;
	qboolean seenDrawSurfs = qfalse;

	while ( 1 ) {
		switch ( *(int *)curCmd ) {
		case RC_SET_COLOR:
			curCmd = (void *)( (setColorCommand_t *)curCmd + 1 );
			break;
		case RC_STRETCH_PIC:
		case RC_STRETCH_PIC_GRADIENT:
			if ( seenDrawSurfs ) {
				R_VR_PatchStretchPicCommand( (stretchPicCommand_t *)curCmd, stereoFrame );
			}
			curCmd = (void *)( (stretchPicCommand_t *)curCmd + 1 );
			break;
		case RC_DRAW_SURFS:
			R_VR_PatchDrawSurfsCommand( (drawSurfsCommand_t *)curCmd, stereoFrame );
			seenDrawSurfs = qtrue;
			curCmd = (void *)( (drawSurfsCommand_t *)curCmd + 1 );
			break;
		case RC_DRAW_BUFFER:
			curCmd = (void *)( (drawBufferCommand_t *)curCmd + 1 );
			break;
		case RC_SWAP_BUFFERS:
		case RC_FLUSH:
			curCmd = (void *)( (swapBuffersCommand_t *)curCmd + 1 );
			break;
		case RC_END_OF_LIST:
		default:
			return;
		}
	}
}


/*
=====================
R_PerformanceCounters
=====================
*/
void R_PerformanceCounters( void ) {
	if ( !r_speeds->integer ) {
		// clear the counters even if we aren't printing
		memset( &tr.pc, 0, sizeof( tr.pc ) );
		memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
		return;
	}

	if ( r_speeds->integer == 1 ) {
		ri.Printf( PRINT_ALL, "%i/%i shaders/surfs %i leafs %i verts %i/%i tris %.2f mtex %.2f dc\n",
				   backEnd.pc.c_shaders, backEnd.pc.c_surfaces, tr.pc.c_leafs, backEnd.pc.c_vertexes,
				   backEnd.pc.c_indexes / 3, backEnd.pc.c_totalIndexes / 3,
				   R_SumOfUsedImages() / ( 1000000.0f ), backEnd.pc.c_overDraw / (float)( glConfig.vidWidth * glConfig.vidHeight ) );
	} else if ( r_speeds->integer == 2 ) {
		ri.Printf( PRINT_ALL, "(patch) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
				   tr.pc.c_sphere_cull_patch_in, tr.pc.c_sphere_cull_patch_clip, tr.pc.c_sphere_cull_patch_out,
				   tr.pc.c_box_cull_patch_in, tr.pc.c_box_cull_patch_clip, tr.pc.c_box_cull_patch_out );
		ri.Printf( PRINT_ALL, "(md3) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
				   tr.pc.c_sphere_cull_md3_in, tr.pc.c_sphere_cull_md3_clip, tr.pc.c_sphere_cull_md3_out,
				   tr.pc.c_box_cull_md3_in, tr.pc.c_box_cull_md3_clip, tr.pc.c_box_cull_md3_out );
	} else if ( r_speeds->integer == 3 ) {
		ri.Printf( PRINT_ALL, "viewcluster: %i\n", tr.viewCluster );
	} else if ( r_speeds->integer == 4 ) {
		if ( backEnd.pc.c_dlightVertexes ) {
			ri.Printf( PRINT_ALL, "dlight srf:%i  culled:%i  verts:%i  tris:%i\n",
					   tr.pc.c_dlightSurfaces, tr.pc.c_dlightSurfacesCulled,
					   backEnd.pc.c_dlightVertexes, backEnd.pc.c_dlightIndexes / 3 );
		}
	}
//----(SA)	this is unnecessary since it will always show 2048.  I moved this to where it is accurate for the world
//	else if (r_speeds->integer == 5 )
//	{
//		ri.Printf( PRINT_ALL, "zFar: %.0f\n", tr.viewParms.zFar );
//	}
	else if ( r_speeds->integer == 6 ) {
		ri.Printf( PRINT_ALL, "flare adds:%i tests:%i renders:%i\n",
				   backEnd.pc.c_flareAdds, backEnd.pc.c_flareTests, backEnd.pc.c_flareRenders );
	}

	memset( &tr.pc, 0, sizeof( tr.pc ) );
	memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
}


/*
====================
R_InitCommandBuffers
====================
*/
void R_InitCommandBuffers( void ) {
	glConfig.smpActive = qfalse;
	if ( r_smp->integer ) {
		ri.Printf( PRINT_ALL, "Trying SMP acceleration...\n" );
		if ( GLimp_SpawnRenderThread( RB_RenderThread ) ) {
			ri.Printf( PRINT_ALL, "...succeeded.\n" );
			glConfig.smpActive = qtrue;
		} else {
			ri.Printf( PRINT_ALL, "...failed.\n" );
		}
	}
}

/*
====================
R_ShutdownCommandBuffers
====================
*/
void R_ShutdownCommandBuffers( void ) {
	// kill the rendering thread
	if ( glConfig.smpActive ) {
		GLimp_WakeRenderer( NULL );
		glConfig.smpActive = qfalse;
	}
}

/*
====================
R_IssueRenderCommands
====================
*/
int c_blockedOnRender;
int c_blockedOnMain;

void R_IssueRenderCommands( qboolean runPerformanceCounters ) {
	renderCommandList_t *cmdList;
	vrStereoReplayStats_t stats;
	qboolean statsValid;
	int usedBytes;
	static int issueLogs;

	cmdList = &backEndData[tr.smpFrame]->commands;
	assert( cmdList ); // bk001205
	// add an end-of-list command
	*( int * )( cmdList->cmds + cmdList->used ) = RC_END_OF_LIST;
	usedBytes = cmdList->used;
	statsValid = R_VR_InspectStereoReplayCommands( cmdList->cmds, &stats );
	if ( issueLogs++ < 40 || ( issueLogs % 300 ) == 0 ) {
		ri.Printf( PRINT_ALL,
				   "VR render issue: bytes=%d valid=%d drawBuffers=%d drawSurfs=%d stretchPics=%d flushes=%d swaps=%d perf=%d frame=%d smp=%d\n",
				   usedBytes,
				   statsValid,
				   stats.drawBuffers,
				   stats.drawSurfs,
				   stats.stretchPics,
				   stats.flushes,
				   stats.swaps,
				   runPerformanceCounters,
				   tr.frameCount,
				   tr.smpFrame );
	}

	// clear it out, in case this is a sync and not a buffer flip
	cmdList->used = 0;

	if ( glConfig.smpActive ) {
		// if the render thread is not idle, wait for it
		if ( renderThreadActive ) {
			c_blockedOnRender++;
			if ( r_showSmp->integer ) {
				ri.Printf( PRINT_ALL, "R" );
			}
		} else {
			c_blockedOnMain++;
			if ( r_showSmp->integer ) {
				ri.Printf( PRINT_ALL, "." );
			}
		}

		// sleep until the renderer has completed
		GLimp_FrontEndSleep();
	}

	// at this point, the back end thread is idle, so it is ok
	// to look at it's performance counters
	if ( runPerformanceCounters ) {
		R_PerformanceCounters();
	}

	// actually start the commands going
	if ( !r_skipBackEnd->integer ) {
		// let it start on the new batch
		if ( !glConfig.smpActive ) {
			RB_ExecuteRenderCommands( cmdList->cmds );
		} else {
			GLimp_WakeRenderer( cmdList );
		}
	}
}


/*
====================
R_SyncRenderThread

Issue any pending commands and wait for them to complete.
After exiting, the render thread will have completed its work
and will remain idle and the main thread is free to issue
OpenGL calls until R_IssueRenderCommands is called.
====================
*/
void R_SyncRenderThread( void ) {
	if ( !tr.registered ) {
		return;
	}
	R_IssueRenderCommands( qfalse );

	if ( !glConfig.smpActive ) {
		return;
	}
	GLimp_FrontEndSleep();
}

/*
============
R_GetCommandBuffer

make sure there is enough command space, waiting on the
render thread if needed.
============
*/
void *R_GetCommandBuffer( int bytes ) {
	renderCommandList_t *cmdList;

	cmdList = &backEndData[tr.smpFrame]->commands;

	// always leave room for the end of list command
	if ( cmdList->used + bytes + 4 > MAX_RENDER_COMMANDS ) {
		if ( bytes > MAX_RENDER_COMMANDS - 4 ) {
			ri.Error( ERR_FATAL, "R_GetCommandBuffer: bad size %i", bytes );
		}
		// if we run out of room, just start dropping commands
		return NULL;
	}

	cmdList->used += bytes;

	return cmdList->cmds + cmdList->used - bytes;
}


/*
=============
R_AddDrawSurfCmd

=============
*/
void    R_AddDrawSurfCmd( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	drawSurfsCommand_t  *cmd;

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_DRAW_SURFS;

	cmd->drawSurfs = drawSurfs;
	cmd->numDrawSurfs = numDrawSurfs;

	cmd->refdef = tr.refdef;
	cmd->viewParms = tr.viewParms;
}


/*
=============
RE_SetColor

Passing NULL will set the color to white
=============
*/
void    RE_SetColor( const float *rgba ) {
	setColorCommand_t   *cmd;

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SET_COLOR;
	if ( !rgba ) {
		static float colorWhite[4] = { 1, 1, 1, 1 };

		rgba = colorWhite;
	}

	cmd->color[0] = rgba[0];
	cmd->color[1] = rgba[1];
	cmd->color[2] = rgba[2];
	cmd->color[3] = rgba[3];
}


/*
=============
RE_StretchPic
=============
*/
void RE_StretchPic( float x, float y, float w, float h,
					float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	stretchPicCommand_t *cmd;

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_STRETCH_PIC;
	cmd->shader = R_GetShaderByHandle( hShader );
	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;
	cmd->s1 = s1;
	cmd->t1 = t1;
	cmd->s2 = s2;
	cmd->t2 = t2;
}


//----(SA)	added
/*
==============
RE_StretchPicGradient
==============
*/
void RE_StretchPicGradient( float x, float y, float w, float h,
							float s1, float t1, float s2, float t2, qhandle_t hShader, const float *gradientColor, int gradientType ) {
	stretchPicCommand_t *cmd;

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_STRETCH_PIC_GRADIENT;
	cmd->shader = R_GetShaderByHandle( hShader );
	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;
	cmd->s1 = s1;
	cmd->t1 = t1;
	cmd->s2 = s2;
	cmd->t2 = t2;

	if ( !gradientColor ) {
		static float colorWhite[4] = { 1, 1, 1, 1 };

		gradientColor = colorWhite;
	}

	cmd->gradientColor[0] = gradientColor[0] * 255;
	cmd->gradientColor[1] = gradientColor[1] * 255;
	cmd->gradientColor[2] = gradientColor[2] * 255;
	cmd->gradientColor[3] = gradientColor[3] * 255;
	cmd->gradientType = gradientType;
}
//----(SA)	end


/*
====================
RE_BeginFrame

If running in stereo, RE_BeginFrame will be called twice
for each RE_EndFrame
====================
*/
void RE_BeginFrame( stereoFrame_t stereoFrame ) {
	drawBufferCommand_t *cmd;
	static int beginLogs;

	if ( !tr.registered ) {
		return;
	}
	glState.finishCalled = qfalse;

	tr.frameCount++;
	tr.frameSceneNum = 0;
	if ( beginLogs++ < 40 || ( beginLogs % 300 ) == 0 ) {
		GLint fb = 0;
		qglGetIntegerv( GL_FRAMEBUFFER_BINDING, &fb );
		ri.Printf( PRINT_ALL,
				   "VR RE_BeginFrame: stereo=%d frame=%d fbo=%d vid=%dx%d\n",
				   stereoFrame,
				   tr.frameCount,
				   fb,
				   glConfig.vidWidth,
				   glConfig.vidHeight );
	}

	//
	// do overdraw measurement
	//
#ifndef HAVE_GLES
	if ( r_measureOverdraw->integer ) {
		if ( glConfig.stencilBits < 4 ) {
			ri.Printf( PRINT_ALL, "Warning: not enough stencil bits to measure overdraw: %d\n", glConfig.stencilBits );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		} else if ( r_shadows->integer == 2 )   {
			ri.Printf( PRINT_ALL, "Warning: stencil shadows and overdraw measurement are mutually exclusive\n" );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		} else
		{
			R_SyncRenderThread();
			qglEnable( GL_STENCIL_TEST );
			qglStencilMask( ~0U );
			qglClearStencil( 0U );
			qglStencilFunc( GL_ALWAYS, 0U, ~0U );
			qglStencilOp( GL_KEEP, GL_INCR, GL_INCR );
		}
		r_measureOverdraw->modified = qfalse;
	} else
	{
		// this is only reached if it was on and is now off
		if ( r_measureOverdraw->modified ) {
			R_SyncRenderThread();
			qglDisable( GL_STENCIL_TEST );
		}
		r_measureOverdraw->modified = qfalse;
	}
#endif

	//
	// texturemode / anisotropy stuff
	//
	if ( r_textureMode->modified  ||  r_ext_texture_filter_anisotropic->modified ) {
		R_SyncRenderThread();
		GL_TextureMode( r_textureMode->string );
		r_textureMode->modified = qfalse;
		r_ext_texture_filter_anisotropic->modified = qfalse;
	}

	//
	// ATI stuff
	//
#ifndef HAVE_GLES
	// TRUFORM
	if ( qglPNTrianglesiATI ) {

		// tess
		if ( r_ati_truform_tess->modified ) {
			r_ati_truform_tess->modified = qfalse;
			// cap if necessary
			if ( r_ati_truform_tess->value > glConfig.ATIMaxTruformTess ) {
				ri.Cvar_Set( "r_ati_truform_tess", va( "%d",glConfig.ATIMaxTruformTess ) );
			}

			qglPNTrianglesiATI( GL_PN_TRIANGLES_TESSELATION_LEVEL_ATI, r_ati_truform_tess->value );
		}

		// point mode
		if ( r_ati_truform_pointmode->modified ) {
			r_ati_truform_pointmode->modified = qfalse;
			// GR - shorten the mode name
			if ( !Q_stricmp( r_ati_truform_pointmode->string, "LINEAR" ) ) {
				glConfig.ATIPointMode = (int)GL_PN_TRIANGLES_POINT_MODE_LINEAR_ATI;
				// GR - fix point mode change
			} else if ( !Q_stricmp( r_ati_truform_pointmode->string, "CUBIC" ) ) {
				glConfig.ATIPointMode = (int)GL_PN_TRIANGLES_POINT_MODE_CUBIC_ATI;
			} else {
				// bogus value, set to valid
				glConfig.ATIPointMode = (int)GL_PN_TRIANGLES_POINT_MODE_CUBIC_ATI;
				ri.Cvar_Set( "r_ati_truform_pointmode", "LINEAR" );
			}
			qglPNTrianglesiATI( GL_PN_TRIANGLES_POINT_MODE_ATI, glConfig.ATIPointMode );
		}

		// normal mode
		if ( r_ati_truform_normalmode->modified ) {
			r_ati_truform_normalmode->modified = qfalse;
			// GR - shorten the mode name
			if ( !Q_stricmp( r_ati_truform_normalmode->string, "LINEAR" ) ) {
				glConfig.ATINormalMode = (int)GL_PN_TRIANGLES_NORMAL_MODE_LINEAR_ATI;
				// GR - fix normal mode change
			} else if ( !Q_stricmp( r_ati_truform_normalmode->string, "QUADRATIC" ) ) {
				glConfig.ATINormalMode = (int)GL_PN_TRIANGLES_NORMAL_MODE_QUADRATIC_ATI;
			} else {
				// bogus value, set to valid
				glConfig.ATINormalMode = (int)GL_PN_TRIANGLES_NORMAL_MODE_LINEAR_ATI;
				ri.Cvar_Set( "r_ati_truform_normalmode", "LINEAR" );
			}
			qglPNTrianglesiATI( GL_PN_TRIANGLES_NORMAL_MODE_ATI, glConfig.ATINormalMode );
		}
	}

	//
	// NVidia stuff
	//

	// fog control
	if ( glConfig.NVFogAvailable && r_nv_fogdist_mode->modified ) {
		r_nv_fogdist_mode->modified = qfalse;
		if ( !Q_stricmp( r_nv_fogdist_mode->string, "GL_EYE_PLANE_ABSOLUTE_NV" ) ) {
			glConfig.NVFogMode = (int)GL_EYE_PLANE_ABSOLUTE_NV;
		} else if ( !Q_stricmp( r_nv_fogdist_mode->string, "GL_EYE_PLANE" ) ) {
			glConfig.NVFogMode = (int)GL_EYE_PLANE;
		} else if ( !Q_stricmp( r_nv_fogdist_mode->string, "GL_EYE_RADIAL_NV" ) ) {
			glConfig.NVFogMode = (int)GL_EYE_RADIAL_NV;
		} else {
			// in case this was really 'else', store a valid value for next time
			glConfig.NVFogMode = (int)GL_EYE_RADIAL_NV;
			ri.Cvar_Set( "r_nv_fogdist_mode", "GL_EYE_RADIAL_NV" );
		}
	}
#endif

	//
	// gamma stuff
	//
	if ( r_gamma->modified ) {
		r_gamma->modified = qfalse;

		R_SyncRenderThread();
		R_SetColorMappings();
	}

	// check for errors
	if ( !r_ignoreGLErrors->integer ) {
		int err;

		R_SyncRenderThread();
		if ( ( err = qglGetError() ) != GL_NO_ERROR ) {
			ri.Error( ERR_FATAL, "RE_BeginFrame() - glGetError() failed (0x%x)!\n", err );
		}
	}

	//
	// draw buffer stuff
	//
	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_DRAW_BUFFER;

	{
		if ( stereoFrame == STEREO_LEFT || stereoFrame == STEREO_CENTER ) {
			cmd->buffer = (int)0;
		} else if ( stereoFrame == STEREO_RIGHT ) {
			cmd->buffer = (int)1;
		} else {
			ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is enabled, but stereoFrame was %i", stereoFrame );
		}
	}

/*
#ifndef HAVE_GLES
	if ( glConfig.stereoEnabled ) {
		if ( stereoFrame == STEREO_LEFT ) {
			cmd->buffer = (int)GL_BACK_LEFT;
		} else if ( stereoFrame == STEREO_RIGHT ) {
			cmd->buffer = (int)GL_BACK_RIGHT;
		} else {
			ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is enabled, but stereoFrame was %i", stereoFrame );
		}
	} else {
		if ( stereoFrame != STEREO_CENTER ) {
			ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is disabled, but stereoFrame was %i", stereoFrame );
		}
		if ( !Q_stricmp( r_drawBuffer->string, "GL_FRONT" ) ) {
			cmd->buffer = (int)GL_FRONT;
		} else {
#endif
			cmd->buffer = (int)GL_BACK;
#ifndef HAVE_GLES
		}
	}
#endif
*/
}


/*
=============
RE_EndFrame

Returns the number of msec spent in the back end
=============
*/
void RE_EndFrame( int stereoFrame, int *frontEndMsec, int *backEndMsec ) {

    swapBuffersCommand_t    *cmd;
	static int endLogs;

    if ( !tr.registered ) {
        return;
    }

    cmd = R_GetCommandBuffer(sizeof(*cmd));
    if (!cmd) {
        return;
    }

    cmd->commandId = RC_FLUSH;
	if ( endLogs++ < 40 || ( endLogs % 300 ) == 0 ) {
		GLint fb = 0;
		qglGetIntegerv( GL_FRAMEBUFFER_BINDING, &fb );
		ri.Printf( PRINT_ALL,
				   "VR RE_EndFrame: stereo=%d frame=%d fbo=%d\n",
				   stereoFrame,
				   tr.frameCount,
				   fb );
	}

    R_IssueRenderCommands( qfalse );

    if (frontEndMsec) {
        *frontEndMsec = tr.frontEndMsec;
    }
    tr.frontEndMsec = 0;
    if (backEndMsec) {
        *backEndMsec = backEnd.pc.msec;
    }
    backEnd.pc.msec = 0;
}


/*
=============
RE_EndFrame

Returns the number of msec spent in the back end
=============
*/
void RE_SubmitStereoFrame( ) {
    swapBuffersCommand_t    *cmd;
	static int submitLogs;

    if ( !tr.registered ) {
        return;
    }

    cmd = R_GetCommandBuffer(sizeof(*cmd));
    if (!cmd) {
        return;
    }

    cmd->commandId = RC_SWAP_BUFFERS;
	if ( submitLogs++ < 40 || ( submitLogs % 300 ) == 0 ) {
		GLint fb = 0;
		qglGetIntegerv( GL_FRAMEBUFFER_BINDING, &fb );
		ri.Printf( PRINT_ALL,
				   "VR RE_SubmitStereoFrame: frame=%d fbo=%d\n",
				   tr.frameCount,
				   fb );
	}

    R_IssueRenderCommands( qtrue );

    // use the other buffers next frame, because another CPU
    // may still be rendering into the current ones
    R_ToggleSmpFrame();
}

qboolean RE_VR_BeginStereoReplayCapture( void ) {
	if ( !tr.registered ) {
		return qfalse;
	}

	vrStereoReplayActive = qfalse;
	vrStereoReplayCommandBytes = 0;
	return qtrue;
}

void RE_VR_CancelStereoReplayCapture( void ) {
	vrStereoReplayActive = qfalse;
	vrStereoReplayCommandBytes = 0;
	R_ToggleSmpFrame();
}

qboolean RE_VR_ReplayStereoFrame( stereoFrame_t stereoFrame, qboolean finalReplay ) {
	renderCommandList_t *cmdList;
	swapBuffersCommand_t *cmd;
	vrStereoReplayStats_t stats;
	static qboolean loggedReplayStats = qfalse;

	if ( !tr.registered ) {
		return qfalse;
	}

	cmdList = &backEndData[tr.smpFrame]->commands;

	if ( !vrStereoReplayActive ) {
		cmd = R_GetCommandBuffer( sizeof( *cmd ) );
		if ( !cmd ) {
			RE_VR_CancelStereoReplayCapture();
			return qfalse;
		}
		cmd->commandId = RC_SWAP_BUFFERS;

		*( int * )( cmdList->cmds + cmdList->used ) = RC_END_OF_LIST;
		vrStereoReplayCommandBytes = cmdList->used + sizeof( int );
		if ( vrStereoReplayCommandBytes > (int)sizeof( vrStereoReplayCommands.cmds ) ) {
			RE_VR_CancelStereoReplayCapture();
			return qfalse;
		}

		memcpy( vrStereoReplayCommands.cmds, cmdList->cmds, vrStereoReplayCommandBytes );
		if ( !R_VR_InspectStereoReplayCommands( vrStereoReplayCommands.cmds, &stats ) ||
			 ( stats.drawSurfs == 0 && stats.stretchPics == 0 ) ) {
			if ( !vrStereoReplayLoggedInvalid ) {
				ri.Printf( PRINT_WARNING,
						   "VR stereo replay: invalid captured command stream (%d bytes, drawSurfs=%d, stretchPics=%d, swaps=%d).\n",
						   vrStereoReplayCommandBytes, stats.drawSurfs, stats.stretchPics, stats.swaps );
				vrStereoReplayLoggedInvalid = qtrue;
			}
			RE_VR_CancelStereoReplayCapture();
			return qfalse;
		}
		if ( !loggedReplayStats ) {
			ri.Printf( PRINT_ALL,
					   "VR stereo replay: captured %d bytes (%d drawSurfs, %d stretchPics, %d swaps).\n",
					   vrStereoReplayCommandBytes, stats.drawSurfs, stats.stretchPics, stats.swaps );
			loggedReplayStats = qtrue;
		}
		vrStereoReplayActive = qtrue;
		cmdList->used = 0;
	}

	memcpy( vrStereoReplayScratch.cmds, vrStereoReplayCommands.cmds, vrStereoReplayCommandBytes );
	R_VR_PatchStereoReplayCommands( vrStereoReplayScratch.cmds, stereoFrame );

	if ( !r_skipBackEnd->integer ) {
		RB_ExecuteRenderCommands( vrStereoReplayScratch.cmds );
	}

	if ( finalReplay ) {
		R_PerformanceCounters();
		R_ToggleSmpFrame();
		vrStereoReplayActive = qfalse;
		vrStereoReplayCommandBytes = 0;
		tr.frontEndMsec = 0;
		backEnd.pc.msec = 0;
	}

	return qtrue;
}
