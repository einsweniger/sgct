/*************************************************************************
Copyright (c) 2012 Miroslav Andel, Link�ping University.
All rights reserved.

Original Authors:
Miroslav Andel, Alexander Fridlund

For any questions or information about the SGCT project please contact: miroslav.andel@liu.se

This work is licensed under the Creative Commons Attribution-ShareAlike 3.0 Unported License.
To view a copy of this license, visit http://creativecommons.org/licenses/by-sa/3.0/ or send a letter to
Creative Commons, 444 Castro Street, Suite 900, Mountain View, California, 94041, USA.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.
*************************************************************************/

#ifdef __WIN32__
	#define WIN32_LEAN_AND_MEAN
	//prevent conflict between max() in limits.h and max script in windef.h
	#define NOMINMAX
#endif

#include "../include/sgct/Engine.h"
#include "../include/sgct/freetype.h"
#include "../include/sgct/FontManager.h"
#include "../include/sgct/MessageHandler.h"
#include "../include/sgct/TextureManager.h"
#include "../include/sgct/SharedData.h"
#include "../include/sgct/ShaderManager.h"
#include "../include/sgct/ogl_headers.h"
#include "../include/sgct/SGCTInternalShaders.h"
#include "../include/sgct/Image.h"
#include <math.h>
#include <iostream>
#include <sstream>
#include <deque>

using namespace core_sgct;

sgct::Engine *  sgct::Engine::mThis     = NULL;

#ifdef GLEW_MX
GLEWContext * glewGetContext();
#endif

sgct::Engine::Engine( int& argc, char**& argv )
{
	//init pointers
	mThis = this;
	mNetworkConnections = NULL;
	mConfig = NULL;

	//init function pointers
	mDrawFn = NULL;
	mPreSyncFn = NULL;
	mPostSyncPreDrawFn = NULL;
	mPostDrawFn = NULL;
	mInitOGLFn = NULL;
	mClearBufferFn = NULL;
	mCleanUpFn = NULL;
	mInternalRenderFn = NULL;
	mNetworkCallbackFn = NULL;
	mKeyboardCallbackFn = NULL;
	mCharCallbackFn = NULL;
	mMouseButtonCallbackFn = NULL;
	mMousePosCallbackFn = NULL;
	mMouseWheelCallbackFn = NULL;

	mTerminate = false;
	mIgnoreSync = false;

	localRunningMode = NetworkManager::NotLocal;

	currentViewportCoords[0] = 0;
	currentViewportCoords[1] = 0;
	currentViewportCoords[2] = 640;
	currentViewportCoords[3] = 480;

	//FBO stuff
	mFrameBuffers[0] = 0;
	mFrameBuffers[1] = 0;
	mMultiSampledFrameBuffers[0] = 0;
	mMultiSampledFrameBuffers[1] = 0;
	mFrameBufferTextures[0] = 0;
	mFrameBufferTextures[1] = 0;
	mRenderBuffers[0] = 0;
	mRenderBuffers[1] = 0;
	mDepthBuffers[0] = 0;
	mDepthBuffers[1] = 0;
	mFrameBufferTextureLocs[0] = -1;
	mFrameBufferTextureLocs[1] = -1;
	mFBOMode = MultiSampledFBO;

	// Initialize GLFW
	if( !glfwInit() )
	{
		mTerminate = true;
		return;
	}

    NetworkManager::gMutex = createMutex();
	NetworkManager::gSyncMutex = createMutex();
    NetworkManager::gCond = createCondition();

    if(NetworkManager::gMutex == NULL ||
		NetworkManager::gSyncMutex == NULL ||
		NetworkManager::gCond == NULL)
    {
		mTerminate = true;
		return;
	}

	setClearBufferFunction( clearBuffer );
	mNearClippingPlaneDist = 0.1f;
	mFarClippingPlaneDist = 100.0f;
	mShowInfo = false;
	mShowGraph = false;
	mShowWireframe = false;
	mTakeScreenshot = false;
	mActiveFrustum = Frustum::Mono;
    mTimerID = 0;

	//parse needs to be before read config since the path to the XML is parsed here
	parseArguments( argc, argv );
}

bool sgct::Engine::init()
{
	if(mTerminate)
	{
		sgct::MessageHandler::Instance()->print("Failed to init GLFW.\n");
		return false;
	}
	
	mConfig = new ReadConfig( configFilename );
	if( !mConfig->isValid() ) //fatal error
	{
		sgct::MessageHandler::Instance()->print("Error in xml config file parsing.\n");
		return false;
	}
	
	if( !initNetwork() )
	{
		sgct::MessageHandler::Instance()->print("Network init error.\n");
		return false;
	}

	if( !initWindow() )
	{
		sgct::MessageHandler::Instance()->print("Window init error.\n");
		return false;
	}

	if( mKeyboardCallbackFn != NULL )
        glfwSetKeyCallback( mKeyboardCallbackFn );
	if( mMouseButtonCallbackFn != NULL )
		glfwSetMouseButtonCallback( mMouseButtonCallbackFn );
	if( mMousePosCallbackFn != NULL )
		glfwSetMousePosCallback( mMousePosCallbackFn );
	if( mCharCallbackFn != NULL )
		glfwSetCharCallback( mCharCallbackFn );
	if( mMouseWheelCallbackFn != NULL )
		glfwSetMouseWheelCallback( mMouseWheelCallbackFn );

	initOGL();

	return true;
}

bool sgct::Engine::initNetwork()
{
	try
	{
		mNetworkConnections = new NetworkManager(localRunningMode);

	}
	catch(const char * err)
	{
		sgct::MessageHandler::Instance()->print("Initiating network connections failed! Error: '%s'\n", err);
		return false;
	}
	
	//check in cluster configuration which it is
	if( localRunningMode == NetworkManager::NotLocal )
		for(unsigned int i=0; i<ClusterManager::Instance()->getNumberOfNodes(); i++)
			if( mNetworkConnections->matchAddress( ClusterManager::Instance()->getNodePtr(i)->ip ) )
			{
				ClusterManager::Instance()->setThisNodeId(i);
				break;
			}

	if( ClusterManager::Instance()->getThisNodeId() == -1 ||
		ClusterManager::Instance()->getThisNodeId() >= static_cast<int>( ClusterManager::Instance()->getNumberOfNodes() )) //fatal error
	{
		sgct::MessageHandler::Instance()->print("This computer is not a part of the cluster configuration!\n");
		mNetworkConnections->close();
		return false;
	}
	else
	{
		printNodeInfo( static_cast<unsigned int>(ClusterManager::Instance()->getThisNodeId()) );
	}

	//Set message handler to send messages or not
	sgct::MessageHandler::Instance()->sendMessagesToServer( !mNetworkConnections->isComputerServer() );

	if(!mNetworkConnections->init())
		return false;

    sgct::MessageHandler::Instance()->print("Done\n");
	return true;
}

bool sgct::Engine::initWindow()
{
	int tmpGlfwVer[3];
    glfwGetVersion( &tmpGlfwVer[0], &tmpGlfwVer[1], &tmpGlfwVer[2] );
	sgct::MessageHandler::Instance()->print("Using GLFW version %d.%d.%d.\n",
                                         tmpGlfwVer[0],
                                         tmpGlfwVer[1],
                                         tmpGlfwVer[2]);

	getWindowPtr()->useQuadbuffer( ClusterManager::Instance()->getThisNodePtr()->stereo == ReadConfig::Active );

	int antiAliasingSamples = ClusterManager::Instance()->getThisNodePtr()->numberOfSamples;
	if( antiAliasingSamples > 1 && mFBOMode == NoFBO ) //if multisample is used
		glfwOpenWindowHint( GLFW_FSAA_SAMPLES, antiAliasingSamples );
	else if( antiAliasingSamples < 2 && mFBOMode == MultiSampledFBO ) //on sample or less => no multisampling
		mFBOMode = RegularFBO;

    //glfwOpenWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    //glfwOpenWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    //glfwOpenWindowHint( GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE );
    //glfwOpenWindowHint( GLFW_OPENGL_VERSION_MAJOR, 3 );
    //glfwOpenWindowHint( GLFW_OPENGL_VERSION_MINOR, 2 );

	if( !getWindowPtr()->openWindow() )
		return false;

	GLenum err = glewInit();
	if (GLEW_OK != err)
	{
	  //Problem: glewInit failed, something is seriously wrong.
	  sgct::MessageHandler::Instance()->print("Error: %s.\n", glewGetErrorString(err));
	  return false;
	}
	sgct::MessageHandler::Instance()->print("Using GLEW %s.\n", glewGetString(GLEW_VERSION));

    /*
        Swap inerval:

        0 = vertical sync off
        1 = wait for vertical sync
        2 = fix when using swapgroups in xp and running half the framerate
    */

    glfwSwapInterval( ClusterManager::Instance()->getThisNodePtr()->swapInterval );

	//Also join swap group if enabled
	getWindowPtr()->init();

	getWindowPtr()->setWindowTitle( getBasicInfo() );

	waitForAllWindowsInSwapGroupToOpen();

	return true;
}

void sgct::Engine::initOGL()
{
	//Get OpenGL version
	int version[3];
	glfwGetGLVersion( &version[0], &version[1], &version[2] );
	sgct::MessageHandler::Instance()->print("OpenGL version %d.%d.%d\n", version[0], version[1], version[2]);

	if (!GLEW_ARB_texture_non_power_of_two)
	{
		sgct::MessageHandler::Instance()->print("Warning! Only power of two textures are supported!\n");
	}

	ClusterManager::Instance()->updateSceneTransformation(
		mConfig->getYaw(),
		mConfig->getPitch(),
		mConfig->getRoll(),
		(*mConfig->getSceneOffset()));
	createFBOs();
	loadShaders();

	//load overlays if any
	SGCTNode * tmpNode = ClusterManager::Instance()->getThisNodePtr();
	for(unsigned int i=0; i<tmpNode->getNumberOfViewports(); i++)
		tmpNode->getViewport(i)->loadOverlayTexture();

	if( mInitOGLFn != NULL )
		mInitOGLFn();

	calculateFrustums();
	mInternalRenderFn = &Engine::draw;

	//
	// Add fonts
	//
#if __WIN32__
	if( !FontManager::Instance()->AddFont( "Verdana", "verdanab.ttf" ) )
#elif __APPLE__
	if( !FontManager::Instance()->AddFont( "Verdana", "Verdana Bold.ttf" ) )
#else
    if( !FontManager::Instance()->AddFont( "Verdana", "FreeSansBold.ttf" ) )
#endif
		FontManager::Instance()->GetFont( "Verdana", 10 );

	//init swap group barrier when ready to render
	sgct::MessageHandler::Instance()->print("Joining swap barrier if enabled...\n");
	getWindowPtr()->setBarrier(true);

	sgct::MessageHandler::Instance()->print("Reseting swap group frame number...\n");
	getWindowPtr()->resetSwapGroupFrameNumber();

	sgct::MessageHandler::Instance()->print("\nReady to render!\n");
}

void sgct::Engine::clean()
{
	sgct::MessageHandler::Instance()->print("Cleaning up...\n");

	if( mCleanUpFn != NULL )
		mCleanUpFn();

	sgct::MessageHandler::Instance()->print("Clearing all callbacks...\n");
	clearAllCallbacks();

	//delete FBO stuff
	if(mFBOMode != NoFBO && GLEW_EXT_framebuffer_object)
	{
		sgct::MessageHandler::Instance()->print("Releasing OpenGL buffers...\n");
		glDeleteFramebuffersEXT(2,	&mFrameBuffers[0]);
		if(mFBOMode == MultiSampledFBO && GLEW_EXT_framebuffer_multisample)
			glDeleteFramebuffersEXT(2,	&mMultiSampledFrameBuffers[0]);
		glDeleteTextures(2,			&mFrameBufferTextures[0]);
		glDeleteRenderbuffersEXT(2, &mRenderBuffers[0]);
		glDeleteRenderbuffersEXT(2, &mDepthBuffers[0]);
	}

	sgct::ShaderManager::Destroy();

	//de-init window and unbind swapgroups...
	if(ClusterManager::Instance()->getNumberOfNodes() > 0)
	{
		SGCTNode * nPtr = ClusterManager::Instance()->getThisNodePtr();
		if(nPtr != NULL && nPtr->getWindowPtr() != NULL) //make shure to not use destroyed object
			nPtr->getWindowPtr()->close();
	}

	//close TCP connections
	if( mNetworkConnections != NULL )
	{
		delete mNetworkConnections;
		mNetworkConnections = NULL;
	}

	if( mConfig != NULL )
	{
		delete mConfig;
		mConfig = NULL;
	}

	// Destroy explicitly to avoid memory leak messages
	sgct::MessageHandler::Instance()->print("Destroying font manager...\n");
	FontManager::Destroy();
	sgct::MessageHandler::Instance()->print("Destroying shader manager...\n");
	ShaderManager::Destroy();
	sgct::MessageHandler::Instance()->print("Destroying shared data...\n");
	SharedData::Destroy();
	sgct::MessageHandler::Instance()->print("Destroying texture manager...\n");
	TextureManager::Destroy();
	sgct::MessageHandler::Instance()->print("Destroying cluster manager...\n");
	ClusterManager::Destroy();

    sgct::MessageHandler::Instance()->print("Destroying network mutex...\n");
	if( NetworkManager::gMutex != NULL )
	{
		destroyMutex( NetworkManager::gMutex );
		NetworkManager::gMutex = NULL;
	}

	sgct::MessageHandler::Instance()->print("Destroying sync mutex...\n");
	if( NetworkManager::gSyncMutex != NULL )
	{
		destroyMutex( NetworkManager::gSyncMutex );
		NetworkManager::gSyncMutex = NULL;
	}

	sgct::MessageHandler::Instance()->print("Destroying condition...\n");
	if( NetworkManager::gCond != NULL )
	{
		destroyCond( NetworkManager::gCond );
		NetworkManager::gCond = NULL;
	}

	sgct::MessageHandler::Instance()->print("Destroying message handler...\n");
	MessageHandler::Destroy();

	// Close window and terminate GLFW
	std::cout << std::endl << "Terminating glfw...";
	glfwTerminate();
	std::cout << " Done." << std::endl;
}

void sgct::Engine::clearAllCallbacks()
{
	glfwSetKeyCallback( NULL );
	glfwSetMouseButtonCallback( NULL );
	glfwSetMousePosCallback( NULL );
	glfwSetCharCallback( NULL );
	glfwSetMouseWheelCallback( NULL );

	mDrawFn = NULL;
	mPreSyncFn = NULL;
	mPostSyncPreDrawFn = NULL;
	mPostDrawFn = NULL;
	mInitOGLFn = NULL;
	mClearBufferFn = NULL;
	mCleanUpFn = NULL;
	mInternalRenderFn = NULL;
	mNetworkCallbackFn = NULL;
	mKeyboardCallbackFn = NULL;
	mCharCallbackFn = NULL;
	mMouseButtonCallbackFn = NULL;
	mMousePosCallbackFn = NULL;
	mMouseWheelCallbackFn = NULL;

	for(unsigned int i=0; i < mTimers.size(); i++)
	{
		mTimers[i].mCallback = NULL;
	}
}

void sgct::Engine::frameSyncAndLock(sgct::Engine::SyncStage stage)
{
	if(mIgnoreSync)
		return;

	double t0 = glfwGetTime();
	static double syncTime = 0.0;

	if( stage == PreStage )
	{
		mNetworkConnections->sync();

		//run only on clients/slaves
		if( !mNetworkConnections->isComputerServer() ) //not server
		{
			glfwLockMutex( NetworkManager::gSyncMutex );
			while(mNetworkConnections->isRunning() && mRunning)
			{
				if( mNetworkConnections->isSyncComplete() )
						break;

				//release lock once per second
				glfwWaitCond( NetworkManager::gCond,
					NetworkManager::gSyncMutex,
					1.0 );
			}

			glfwUnlockMutex( NetworkManager::gSyncMutex );
		}

		syncTime = glfwGetTime() - t0;
	}
	else //post stage
	{
		if( mNetworkConnections->isComputerServer() &&
			mConfig->isMasterSyncLocked() &&
			/*localRunningMode == NetworkManager::NotLocal &&*/
			!getWindowPtr()->isBarrierActive() )//post stage
		{
			glfwLockMutex( NetworkManager::gSyncMutex );
			while(mNetworkConnections->isRunning() && mRunning)
			{
				if( mNetworkConnections->isSyncComplete() )
						break;

				glfwWaitCond( NetworkManager::gCond,
					NetworkManager::gSyncMutex,
					1.0 );
			}

			glfwUnlockMutex( NetworkManager::gSyncMutex );
			syncTime += glfwGetTime() - t0;
		}

		mStatistics.setSyncTime(syncTime);
	}
}

void sgct::Engine::render()
{
	mRunning = GL_TRUE;

	while( mRunning )
	{
		//update tracking data
		//ClusterManager::Instance()->getTrackingPtr()->update();

		if( mPreSyncFn != NULL )
			mPreSyncFn();

		if( mNetworkConnections->isComputerServer() )
		{
			sgct::SharedData::Instance()->encode();
		}
		else
		{
			if( !mNetworkConnections->isRunning() ) //exit if not running
				break;
		}

		frameSyncAndLock(PreStage);

		if( mPostSyncPreDrawFn != NULL )
			mPostSyncPreDrawFn();

		double startFrameTime = glfwGetTime();
		calcFPS(startFrameTime);

		glLineWidth(1.0);
		mShowWireframe ? glPolygonMode( GL_FRONT_AND_BACK, GL_LINE ) : glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

		//check if re-size needed
		if( getWindowPtr()->isWindowResized() )
			resizeFBOs();

		SGCTNode * tmpNode = ClusterManager::Instance()->getThisNodePtr();
		User * usrPtr = ClusterManager::Instance()->getUserPtr();

		//if any stereo type (except passive) then set frustum mode to left eye
		mActiveFrustum = tmpNode->stereo != static_cast<int>(ReadConfig::NoStereo) ? Frustum::StereoLeftEye : Frustum::Mono;
		setRenderTarget(0); //Set correct render target (Backbuffer, FBO etc..)

		//render all viewports for mono or left eye
		for(unsigned int i=0; i<tmpNode->getNumberOfViewports(); i++)
		{
			tmpNode->setCurrentViewport(i);
			//if passive stereo or mono
			if( tmpNode->stereo == ReadConfig::NoStereo )
				mActiveFrustum = tmpNode->getCurrentViewport()->getEye();

			if( tmpNode->getCurrentViewport()->isTracked() )
			{
				tmpNode->getCurrentViewport()->calculateFrustum(
					mActiveFrustum,
					usrPtr->getPosPtr(mActiveFrustum),
					mNearClippingPlaneDist,
					mFarClippingPlaneDist);
			}
			(this->*mInternalRenderFn)();
		}

		//render right eye view port(s)
		if( tmpNode->stereo != ReadConfig::NoStereo )
		{
			mActiveFrustum = Frustum::StereoRightEye;
			setRenderTarget(1); //Set correct render target (Backbuffer, FBO etc..)

			//render all viewports for right eye
			for(unsigned int i=0; i<tmpNode->getNumberOfViewports(); i++)
			{
				tmpNode->setCurrentViewport(i);

				if( tmpNode->getCurrentViewport()->isTracked() )
				{
					tmpNode->getCurrentViewport()->calculateFrustum(
						mActiveFrustum,
						usrPtr->getPosPtr(mActiveFrustum),
						mNearClippingPlaneDist,
						mFarClippingPlaneDist);
				}
				(this->*mInternalRenderFn)();
			}
		}

		//restore
		glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

		//copy AA-buffer to "regular"/non-AA buffer
		if(mFBOMode == MultiSampledFBO && GLEW_EXT_framebuffer_multisample)
		{
			glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, mMultiSampledFrameBuffers[0]); // source
			glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, mFrameBuffers[0]); // dest
			glBlitFramebufferEXT(
				0, 0, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution(),
				0, 0, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution(),
				GL_COLOR_BUFFER_BIT, GL_NEAREST);

			//copy right buffers if used
			if( tmpNode->stereo != ReadConfig::NoStereo )
			{
				glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, mMultiSampledFrameBuffers[1]); // source
				glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, mFrameBuffers[1]); // dest
				glBlitFramebufferEXT(
					0, 0, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution(),
					0, 0, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution(),
					GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}

			//Draw FBO texture here
			renderFBOTexture();
		}
		else if(mFBOMode == RegularFBO && GLEW_EXT_framebuffer_object)
			renderFBOTexture();

        double endFrameTime = glfwGetTime();
		mStatistics.setDrawTime(endFrameTime - startFrameTime);

		//run post frame actions
		if( mPostDrawFn != NULL )
			mPostDrawFn();

		//draw info & stats
		for(unsigned int i=0; i<tmpNode->getNumberOfViewports(); i++)
		{
			tmpNode->setCurrentViewport(i);
			enterCurrentViewport();
			if( mShowGraph )
				mStatistics.draw();
			if( mShowInfo )
				renderDisplayInfo();
		}

        // check all timers if one of them has expired
        if ( isMaster() )
        {
            for( size_t i = 0; i < mTimers.size(); ++i )
            {
                TimerInformation& currentTimer = mTimers[i];
                const double timeSinceLastFiring = endFrameTime - currentTimer.mLastFired;
                if( timeSinceLastFiring > currentTimer.mInterval )
                {
                    currentTimer.mLastFired = endFrameTime;
                    currentTimer.mCallback(currentTimer.mId);
                }
            }
        }

		//take screenshot
		if( mTakeScreenshot )
			captureBuffer();

		//check for errors
		checkForOGLErrors();

		//wait for nodes render before swapping
		frameSyncAndLock(PostStage);

		//swap frame id to keep track of sync
		mNetworkConnections->swapData();

		// Swap front and back rendering buffers
		glfwSwapBuffers();

		// Check if ESC key was pressed or window was closed
		mRunning = !glfwGetKey( GLFW_KEY_ESC ) && glfwGetWindowParam( GLFW_OPENED ) && !mTerminate;
	}
}

void sgct::Engine::renderDisplayInfo()
{
	SGCTNode * tmpNode = ClusterManager::Instance()->getThisNodePtr();

	glPushAttrib(GL_CURRENT_BIT);
	glColor4f(0.8f,0.8f,0.8f,1.0f);
	unsigned int lFrameNumber = 0;
	getWindowPtr()->getSwapGroupFrameNumber(lFrameNumber);

	glDrawBuffer(GL_BACK); //draw into both back buffers
	Freetype::print(FontManager::Instance()->GetFont( "Verdana", 10 ), 100, 95, "Node ip: %s (%s)",
		tmpNode->ip.c_str(),
		mNetworkConnections->isComputerServer() ? "master" : "slave");
	glColor4f(0.8f,0.8f,0.0f,1.0f);
	Freetype::print(FontManager::Instance()->GetFont( "Verdana", 10 ), 100, 80, "Frame rate: %.3f Hz", mStatistics.getAvgFPS());
	glColor4f(0.8f,0.0f,0.8f,1.0f);
	Freetype::print(FontManager::Instance()->GetFont( "Verdana", 10 ), 100, 65, "Draw time: %.2f ms", getDrawTime()*1000.0);
	glColor4f(0.0f,0.8f,0.8f,1.0f);
	Freetype::print(FontManager::Instance()->GetFont( "Verdana", 10 ), 100, 50, "Sync time (size: %d, comp. ratio: %.3f): %.2f ms",
		SharedData::Instance()->getUserDataSize(),
		SharedData::Instance()->getCompressionRatio(),
		getSyncTime()*1000.0);
	glColor4f(0.8f,0.8f,0.8f,1.0f);
	Freetype::print(FontManager::Instance()->GetFont( "Verdana", 10 ), 100, 35, "Swap groups: %s and %s (%s) | Frame: %d",
		getWindowPtr()->isUsingSwapGroups() ? "Enabled" : "Disabled",
		getWindowPtr()->isBarrierActive() ? "active" : "not active",
		getWindowPtr()->isSwapGroupMaster() ? "master" : "slave",
		lFrameNumber);
	Freetype::print(FontManager::Instance()->GetFont( "Verdana", 10 ), 100, 20, "Tracked: %s | User position: %.3f %.3f %.3f",
		tmpNode->getCurrentViewport()->isTracked() ? "true" : "false",
		getUserPtr()->getXPos(),
		getUserPtr()->getYPos(),
		getUserPtr()->getZPos());

	//if active stereoscopic rendering
	if( tmpNode->stereo == ReadConfig::Active )
	{
		glDrawBuffer(GL_BACK_LEFT);
		Freetype::print( FontManager::Instance()->GetFont( "Verdana", 10 ), 100, 110, "Active eye: Left");
		glDrawBuffer(GL_BACK_RIGHT);
		Freetype::print( FontManager::Instance()->GetFont( "Verdana", 10 ), 100, 110, "Active eye:          Right");
		glDrawBuffer(GL_BACK);
	}
	else //if passive stereo
	{
		if( tmpNode->getCurrentViewport()->getEye() == Frustum::StereoLeftEye )
		{
			Freetype::print( FontManager::Instance()->GetFont( "Verdana", 10 ), 100, 110, "Active eye: Left");
		}
		else if( tmpNode->getCurrentViewport()->getEye() == Frustum::StereoRightEye )
		{
			Freetype::print( FontManager::Instance()->GetFont( "Verdana", 10 ), 100, 110, "Active eye:          Right");
		}
	}
	glPopAttrib();
}

void sgct::Engine::draw()
{
	enterCurrentViewport();

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();

	Viewport * tmpVP = ClusterManager::Instance()->getThisNodePtr()->getCurrentViewport();
	glLoadMatrixf( glm::value_ptr(tmpVP->getProjectionMatrix(mActiveFrustum)) );

	glMatrixMode(GL_MODELVIEW);

	glLoadMatrixf( glm::value_ptr( ClusterManager::Instance()->getSceneTrans() ) );

	if( mDrawFn != NULL )
		mDrawFn();

	//if use overlay
	if( tmpVP->hasOverlayTexture() )
	{
		//enter ortho mode
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glPushMatrix();
		/*
			Some code (using OpenSceneGraph) can mess up the viewport settings.
			To ensure correct mapping enter the current viewport.
		*/
		enterCurrentViewport();
		gluOrtho2D(0.0, 1.0, 0.0, 1.0);

		glMatrixMode(GL_MODELVIEW);

		glPushAttrib( GL_ALL_ATTRIB_BITS );
		glDisable(GL_LIGHTING);
		glDisable(GL_DEPTH_TEST);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glLoadIdentity();

		glColor4f(1.0f,1.0f,1.0f,1.0f);

		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, sgct::TextureManager::Instance()->getTextureByIndex( tmpVP->getOverlayTextureIndex() ) );

		glBegin(GL_QUADS);
		glTexCoord2d(0.0, 0.0);	glVertex2d(0.0, 0.0);
		glTexCoord2d(0.0, 1.0);	glVertex2d(0.0, 1.0);
		glTexCoord2d(1.0, 1.0);	glVertex2d(1.0, 1.0);
		glTexCoord2d(1.0, 0.0);	glVertex2d(1.0, 0.0);
		glEnd();

		glPopAttrib();

		//exit ortho mode
		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
	}
}

void sgct::Engine::setRenderTarget(int bufferIndex)
{
	if(mFBOMode != NoFBO && GLEW_EXT_framebuffer_object)
	{
		//un-bind texture
		glBindTexture(GL_TEXTURE_2D, 0);

		if(mFBOMode == MultiSampledFBO && GLEW_EXT_framebuffer_multisample)
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, mMultiSampledFrameBuffers[bufferIndex]);
		else
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, mFrameBuffers[bufferIndex]);

		setAndClearBuffer(RenderToTexture);
	}
	else
		setAndClearBuffer(BackBuffer);
}

void sgct::Engine::renderFBOTexture()
{
	//unbind framebuffer
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

	//enter ortho mode
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glPushMatrix();
	gluOrtho2D(0.0, static_cast<double>(getWindowPtr()->getHResolution()),
		0.0, static_cast<double>(getWindowPtr()->getVResolution()));

	glMatrixMode(GL_MODELVIEW);

	glPushAttrib(GL_CURRENT_BIT | GL_ENABLE_BIT | GL_TEXTURE_BIT | GL_LIGHTING_BIT );
	glDisable(GL_LIGHTING);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	SGCTNode * tmpNode = ClusterManager::Instance()->getThisNodePtr();

	//clear buffers
	mActiveFrustum = tmpNode->stereo == ReadConfig::Active ? Frustum::StereoLeftEye : Frustum::Mono;
	setAndClearBuffer(BackBuffer);

	glLoadIdentity();

	glViewport (0, 0, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution());

	glColor4f(1.0f,1.0f,1.0f,1.0f);

	if( tmpNode->stereo > ReadConfig::Active )
	{
		switch(tmpNode->stereo)
		{
		case ReadConfig::Anaglyph_Red_Cyan:
			sgct::ShaderManager::Instance()->bindShader( "Anaglyph_Red_Cyan" );
			break;

		case ReadConfig::Anaglyph_Amber_Blue:
			sgct::ShaderManager::Instance()->bindShader( "Anaglyph_Amber_Blue" );
			break;

		case ReadConfig::Checkerboard:
			sgct::ShaderManager::Instance()->bindShader( "Checkerboard" );
			break;

		case ReadConfig::Checkerboard_Inverted:
			sgct::ShaderManager::Instance()->bindShader( "Checkerboard_Inverted" );
			break;
		}

		glUniform1i( mFrameBufferTextureLocs[0], 0);
		glUniform1i( mFrameBufferTextureLocs[1], 1);

		glActiveTextureARB(GL_TEXTURE0_ARB);
		glBindTexture(GL_TEXTURE_2D, mFrameBufferTextures[0]);
		glEnable(GL_TEXTURE_2D);

		glActiveTextureARB(GL_TEXTURE1_ARB);
		glBindTexture(GL_TEXTURE_2D, mFrameBufferTextures[1]);
		glEnable(GL_TEXTURE_2D);

		glBegin(GL_QUADS);
		glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0.0, 0.0);
		glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0.0, 0.0);
		glVertex2i(0, 0);

		glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0.0, 1.0);
		glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0.0, 1.0);
		glVertex2i(0, getWindowPtr()->getVResolution());

		glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 1.0, 1.0);
		glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 1.0, 1.0);
		glVertex2i(getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution());

		glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 1.0, 0.0);
		glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 1.0, 0.0);
		glVertex2i(getWindowPtr()->getHResolution(), 0);

		glEnd();

		sgct::ShaderManager::Instance()->unBindShader();
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, mFrameBufferTextures[0]);
		glEnable(GL_TEXTURE_2D);

		glBegin(GL_QUADS);
		glTexCoord2d(0.0, 0.0);	glVertex2i(0, 0);
		glTexCoord2d(0.0, 1.0);	glVertex2i(0, getWindowPtr()->getVResolution());
		glTexCoord2d(1.0, 1.0);	glVertex2i(getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution());
		glTexCoord2d(1.0, 0.0);	glVertex2i(getWindowPtr()->getHResolution(), 0);
		glEnd();
	}

	//render right eye in active stereo mode
	if( tmpNode->stereo == ReadConfig::Active )
	{
		//clear buffers
		mActiveFrustum = Frustum::StereoRightEye;
		setAndClearBuffer(BackBuffer);

		glLoadIdentity();

		glViewport (0, 0, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution());

		glColor4f(1.0f,1.0f,1.0f,1.0f);
		glBindTexture(GL_TEXTURE_2D, mFrameBufferTextures[1]);

		glBegin(GL_QUADS);
		glTexCoord2d(0.0, 0.0);	glVertex2i(0, 0);
		glTexCoord2d(0.0, 1.0);	glVertex2i(0, getWindowPtr()->getVResolution());
		glTexCoord2d(1.0, 1.0);	glVertex2i(getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution());
		glTexCoord2d(1.0, 0.0);	glVertex2i(getWindowPtr()->getHResolution(), 0);
		glEnd();
	}

	glPopAttrib();

	//exit ortho mode
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
}

void sgct::Engine::loadShaders()
{
	SGCTNode * tmpNode = ClusterManager::Instance()->getThisNodePtr();

	if( tmpNode->stereo == ReadConfig::Anaglyph_Red_Cyan )
	{
		sgct::ShaderManager::Instance()->addShader("Anaglyph_Red_Cyan", core_sgct::shaders::Anaglyph_Vert_Shader, core_sgct::shaders::Anaglyph_Red_Cyan_Frag_Shader, ShaderManager::SHADER_SRC_STRING );
		sgct::ShaderManager::Instance()->bindShader( "Anaglyph_Red_Cyan" );
		mFrameBufferTextureLocs[0] = sgct::ShaderManager::Instance()->getShader( "Anaglyph_Red_Cyan" ).getUniformLocation( "LeftTex" );
		mFrameBufferTextureLocs[1] = sgct::ShaderManager::Instance()->getShader( "Anaglyph_Red_Cyan" ).getUniformLocation( "RightTex" );
		glUniform1i( mFrameBufferTextureLocs[0], 0 );
		glUniform1i( mFrameBufferTextureLocs[1], 1 );
		sgct::ShaderManager::Instance()->unBindShader();
	}
	else if( tmpNode->stereo == ReadConfig::Anaglyph_Amber_Blue )
	{
		sgct::ShaderManager::Instance()->addShader("Anaglyph_Amber_Blue", core_sgct::shaders::Anaglyph_Vert_Shader, core_sgct::shaders::Anaglyph_Amber_Blue_Frag_Shader, ShaderManager::SHADER_SRC_STRING );
		sgct::ShaderManager::Instance()->bindShader( "Anaglyph_Amber_Blue" );
		mFrameBufferTextureLocs[0] = sgct::ShaderManager::Instance()->getShader( "Anaglyph_Amber_Blue" ).getUniformLocation( "LeftTex" );
		mFrameBufferTextureLocs[1] = sgct::ShaderManager::Instance()->getShader( "Anaglyph_Amber_Blue" ).getUniformLocation( "RightTex" );
		glUniform1i( mFrameBufferTextureLocs[0], 0 );
		glUniform1i( mFrameBufferTextureLocs[1], 1 );
		sgct::ShaderManager::Instance()->unBindShader();
	}
	else if( tmpNode->stereo == ReadConfig::Checkerboard )
	{
		sgct::ShaderManager::Instance()->addShader("Checkerboard", core_sgct::shaders::Anaglyph_Vert_Shader, core_sgct::shaders::CheckerBoard_Frag_Shader, ShaderManager::SHADER_SRC_STRING );
		sgct::ShaderManager::Instance()->bindShader( "Checkerboard" );
		mFrameBufferTextureLocs[0] = sgct::ShaderManager::Instance()->getShader( "Checkerboard" ).getUniformLocation( "LeftTex" );
		mFrameBufferTextureLocs[1] = sgct::ShaderManager::Instance()->getShader( "Checkerboard" ).getUniformLocation( "RightTex" );
		glUniform1i( mFrameBufferTextureLocs[0], 0 );
		glUniform1i( mFrameBufferTextureLocs[1], 1 );
		sgct::ShaderManager::Instance()->unBindShader();
	}
	else if( tmpNode->stereo == ReadConfig::Checkerboard_Inverted )
	{
		sgct::ShaderManager::Instance()->addShader("Checkerboard_Inverted", core_sgct::shaders::Anaglyph_Vert_Shader, core_sgct::shaders::CheckerBoard_Inverted_Frag_Shader, ShaderManager::SHADER_SRC_STRING );
		sgct::ShaderManager::Instance()->bindShader( "Checkerboard_Inverted" );
		mFrameBufferTextureLocs[0] = sgct::ShaderManager::Instance()->getShader( "Checkerboard_Inverted" ).getUniformLocation( "LeftTex" );
		mFrameBufferTextureLocs[1] = sgct::ShaderManager::Instance()->getShader( "Checkerboard_Inverted" ).getUniformLocation( "RightTex" );
		glUniform1i( mFrameBufferTextureLocs[0], 0 );
		glUniform1i( mFrameBufferTextureLocs[1], 1 );
		sgct::ShaderManager::Instance()->unBindShader();
	}
}

void sgct::Engine::createFBOs()
{
	if(mFBOMode != NoFBO && GLEW_EXT_framebuffer_object)
	{
		glPushAttrib(GL_CURRENT_BIT | GL_ENABLE_BIT | GL_TEXTURE_BIT);
		glEnable(GL_TEXTURE_2D);
		glGenFramebuffersEXT(2,		&mFrameBuffers[0]);
		glGenRenderbuffersEXT(2,	&mDepthBuffers[0]);
		glGenRenderbuffersEXT(2,	&mRenderBuffers[0]);
		glGenTextures(2,			&mFrameBufferTextures[0]);

		if(mFBOMode == MultiSampledFBO && GLEW_EXT_framebuffer_multisample)
		{
			GLint MaxSamples;
			glGetIntegerv(GL_MAX_SAMPLES_EXT, &MaxSamples);
			if( ClusterManager::Instance()->getThisNodePtr()->numberOfSamples > MaxSamples )
				ClusterManager::Instance()->getThisNodePtr()->numberOfSamples = MaxSamples;
			if( MaxSamples < 2 )
				ClusterManager::Instance()->getThisNodePtr()->numberOfSamples = 0;

			sgct::MessageHandler::Instance()->print("Max samples supported: %d\n", MaxSamples);

			//generate the multisample buffer
			glGenFramebuffersEXT(2, &mMultiSampledFrameBuffers[0]);
		}
		else
			sgct::MessageHandler::Instance()->print("Warning! FBO multisampling is not supported or enabled!\n");

		for( int i=0; i<2; i++ )
		{
			//setup render buffer
			if(mFBOMode == MultiSampledFBO && GLEW_EXT_framebuffer_multisample)
				glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, mMultiSampledFrameBuffers[i]);
			else
				glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, mFrameBuffers[i]);
			glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, mRenderBuffers[i]);
			if(mFBOMode == MultiSampledFBO && GLEW_EXT_framebuffer_multisample)
				glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER_EXT, ClusterManager::Instance()->getThisNodePtr()->numberOfSamples, GL_RGBA, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution());
			else
				glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution());
			glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, mRenderBuffers[i]);

			//setup depth buffer
			if(mFBOMode == MultiSampledFBO && GLEW_EXT_framebuffer_multisample)
				glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, mMultiSampledFrameBuffers[i]);
			else
				glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, mFrameBuffers[i]);
			glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, mDepthBuffers[i]);
			if(mFBOMode == MultiSampledFBO && GLEW_EXT_framebuffer_multisample)
				glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER_EXT, ClusterManager::Instance()->getThisNodePtr()->numberOfSamples, GL_DEPTH_COMPONENT32, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution());
			else
				glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT32, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution());
			glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, mDepthBuffers[i]);

			//setup non-multisample buffer
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, mFrameBuffers[i]);
			glBindTexture(GL_TEXTURE_2D, mFrameBufferTextures[i]);
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution(), 0, GL_RGBA, GL_INT, NULL);
			glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, mFrameBufferTextures[i], 0);
		}

		// Unbind / Go back to regular frame buffer rendering
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

		glPopAttrib();

		sgct::MessageHandler::Instance()->print("FBOs initiated successfully!\n");
	}
	else
	{
		SGCTNode * tmpNode = ClusterManager::Instance()->getThisNodePtr();

		//disable anaglyph & checkerboard stereo if FBOs are not used
		if( tmpNode->stereo > ReadConfig::Active )
			tmpNode->stereo = ReadConfig::NoStereo;
		sgct::MessageHandler::Instance()->print("Warning! FBO rendering is not supported or enabled!\nAnaglyph & checkerboard (DLP) stereo modes are disabled.\n");
	}

}

void sgct::Engine::resizeFBOs()
{
	if(mFBOMode != NoFBO && GLEW_EXT_framebuffer_object)
	{
		//delete all
		glDeleteFramebuffersEXT(2,	&mFrameBuffers[0]);
		if(mFBOMode == MultiSampledFBO && GLEW_EXT_framebuffer_multisample)
			glDeleteFramebuffersEXT(2,	&mMultiSampledFrameBuffers[0]);
		glDeleteTextures(2,			&mFrameBufferTextures[0]);
		glDeleteRenderbuffersEXT(2, &mRenderBuffers[0]);
		glDeleteRenderbuffersEXT(2, &mDepthBuffers[0]);

		//init
		mFrameBuffers[0] = 0;
		mFrameBuffers[1] = 0;
		mMultiSampledFrameBuffers[0] = 0;
		mMultiSampledFrameBuffers[1] = 0;
		mFrameBufferTextures[0] = 0;
		mFrameBufferTextures[1] = 0;
		mRenderBuffers[0] = 0;
		mRenderBuffers[1] = 0;
		mDepthBuffers[0] = 0;
		mDepthBuffers[1] = 0;

		createFBOs();

		sgct::MessageHandler::Instance()->print("FBOs resized successfully!\n");
	}
}

void sgct::Engine::setAndClearBuffer(sgct::Engine::BufferMode mode)
{
	if(mode == BackBuffer)
	{
		SGCTNode * tmpNode = ClusterManager::Instance()->getThisNodePtr();

		//Set buffer
		if( tmpNode->stereo != ReadConfig::Active )
			glDrawBuffer(GL_BACK);
		else if( mActiveFrustum == Frustum::StereoLeftEye )
			glDrawBuffer(GL_BACK_LEFT);
		else if( mActiveFrustum == Frustum::StereoRightEye )
			glDrawBuffer(GL_BACK_RIGHT);
	}

	//clear
	if( mClearBufferFn != NULL )
		mClearBufferFn();
}

void sgct::Engine::checkForOGLErrors()
{
	GLenum oglError = glGetError();

	switch( oglError )
	{
	case GL_INVALID_ENUM:
		sgct::MessageHandler::Instance()->print("OpenGL error: GL_INVALID_ENUM\n");
		break;

	case GL_INVALID_VALUE:
		sgct::MessageHandler::Instance()->print("OpenGL error: GL_INVALID_VALUE\n");
		break;

	case GL_INVALID_OPERATION:
		sgct::MessageHandler::Instance()->print("OpenGL error: GL_INVALID_OPERATION\n");
		break;

	case GL_STACK_OVERFLOW:
		sgct::MessageHandler::Instance()->print("OpenGL error: GL_STACK_OVERFLOW\n");
		break;

	case GL_STACK_UNDERFLOW:
		sgct::MessageHandler::Instance()->print("OpenGL error: GL_STACK_UNDERFLOW\n");
		break;

	case GL_OUT_OF_MEMORY:
		sgct::MessageHandler::Instance()->print("OpenGL error: GL_OUT_OF_MEMORY\n");
		break;

	case GL_TABLE_TOO_LARGE:
		sgct::MessageHandler::Instance()->print("OpenGL error: GL_TABLE_TOO_LARGE\n");
		break;
	}
}

void sgct::Engine::captureBuffer()
{
	double t0 = getTime();

	//allocate
	unsigned char * raw_img = (unsigned char*)malloc( sizeof(unsigned char)*( 4 * getWindowPtr()->getHResolution() * getWindowPtr()->getVResolution() ) );

	static int shotCounter = 0;
	char screenShotFilenameLeft[32];
	char screenShotFilenameRight[32];

	int thisNodeId = ClusterManager::Instance()->getThisNodeId();


#if (_MSC_VER >= 1400) //visual studio 2005 or later
	if( shotCounter < 10 )
	{
		sprintf_s( screenShotFilenameLeft, sizeof(screenShotFilenameLeft), "sgct_node%d_000%d_l.png", thisNodeId, shotCounter);
		sprintf_s( screenShotFilenameRight, sizeof(screenShotFilenameRight), "sgct_node%d_000%d_r.png", thisNodeId, shotCounter);
	}
	else if( shotCounter < 100 )
	{
		sprintf_s( screenShotFilenameLeft, sizeof(screenShotFilenameLeft), "sgct_node%d_00%d_l.png", thisNodeId, shotCounter);
		sprintf_s( screenShotFilenameRight, sizeof(screenShotFilenameRight), "sgct_node%d_00%d_r.png", thisNodeId, shotCounter);
	}
	else if( shotCounter < 1000 )
	{
		sprintf_s( screenShotFilenameLeft, sizeof(screenShotFilenameLeft), "sgct_node%d_0%d_l.png", thisNodeId, shotCounter);
		sprintf_s( screenShotFilenameRight, sizeof(screenShotFilenameRight), "sgct_node%d_0%d_r.png", thisNodeId, shotCounter);
	}
	else if( shotCounter < 10000 )
	{
		sprintf_s( screenShotFilenameLeft, sizeof(screenShotFilenameLeft), "sgct_node%d_%d_l.png", thisNodeId, shotCounter);
		sprintf_s( screenShotFilenameRight, sizeof(screenShotFilenameRight), "sgct_node%d_%d_r.png", thisNodeId, shotCounter);
	}
#else
    if( shotCounter < 10 )
	{
		sprintf( screenShotFilenameLeft, "sgct_node%d_000%d_l.png", thisNodeId, shotCounter);
		sprintf( screenShotFilenameRight, "sgct_node%d_000%d_r.png", thisNodeId, shotCounter);
	}
	else if( shotCounter < 100 )
	{
		sprintf( screenShotFilenameLeft, "sgct_node%d_00%d_l.png", thisNodeId, shotCounter);
		sprintf( screenShotFilenameRight, "sgct_node%d_00%d_r.png", thisNodeId, shotCounter);
	}
	else if( shotCounter < 1000 )
	{
		sprintf( screenShotFilenameLeft, "sgct_node%d_0%d_l.png", thisNodeId, shotCounter);
		sprintf( screenShotFilenameRight, "sgct_node%d_0%d_r.png", thisNodeId, shotCounter);
	}
	else if( shotCounter < 10000 )
	{
		sprintf( screenShotFilenameLeft, "sgct_node%d_%d_l.png", thisNodeId, shotCounter);
		sprintf( screenShotFilenameRight, "sgct_node%d_%d_r.png", thisNodeId, shotCounter);
	}
#endif

	shotCounter++;

	glPushAttrib(GL_ENABLE_BIT | GL_TEXTURE_BIT);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);

	SGCTNode * tmpNode = ClusterManager::Instance()->getThisNodePtr();

	if(mFBOMode != NoFBO && GLEW_EXT_framebuffer_object)
	{
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, mFrameBuffers[0]);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, raw_img);
	}
	else if(tmpNode->stereo == ReadConfig::NoStereo)
	{
		glReadBuffer(GL_FRONT);
		glReadPixels(0, 0, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution(), GL_RGBA, GL_UNSIGNED_BYTE, raw_img);
	}
	else if(tmpNode->stereo == ReadConfig::Active)
	{
		glReadBuffer(GL_FRONT_LEFT);
		glReadPixels(0, 0, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution(), GL_RGBA, GL_UNSIGNED_BYTE, raw_img);
	}

	Image img;
	img.setChannels(4);
	img.setDataPtr( raw_img );
	img.setSize( getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution() );
	img.savePNG(screenShotFilenameLeft);

	if( tmpNode->stereo != ReadConfig::NoStereo )
	{
		if(mFBOMode != NoFBO && GLEW_EXT_framebuffer_object)
		{
			glBindTexture(GL_TEXTURE_2D, mFrameBuffers[1]);
			glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, raw_img);
		}
		else if(tmpNode->stereo == ReadConfig::Active)
		{
			glReadBuffer(GL_FRONT_RIGHT);
			glReadPixels(0, 0, getWindowPtr()->getHResolution(), getWindowPtr()->getVResolution(), GL_RGBA, GL_UNSIGNED_BYTE, raw_img);
		}

		img.savePNG(screenShotFilenameRight);
	}

	img.cleanup();

	glPopAttrib();

	mTakeScreenshot = false;

	sgct::MessageHandler::Instance()->print("Screenshot %d completed in %fs\n", shotCounter, getTime()-t0 );
}

void sgct::Engine::waitForAllWindowsInSwapGroupToOpen()
{
	sgct::MessageHandler::Instance()->print("Joining swap group if enabled/supported...\n");

	//Must wait until all nodes are running if using swap barrier
	if( getWindowPtr()->isUsingSwapGroups() && ClusterManager::Instance()->getNumberOfNodes() > 1)
	{
		//check if swapgroups are supported
		#ifdef __WIN32__
		if( wglewIsSupported("WGL_NV_swap_group") )
			sgct::MessageHandler::Instance()->print("Swap groups are supported by hardware.\n");
		#else
		if( glewIsSupported("GLX_NV_swap_group") )
			sgct::MessageHandler::Instance()->print("Swap groups are supported by hardware.\n");
		#endif
        else
            sgct::MessageHandler::Instance()->print("Swap groups are not supported by hardware.\n");


		sgct::MessageHandler::Instance()->print("Waiting for all nodes to connect.");
		glfwSwapBuffers(); //render just black....

		glfwLockMutex( NetworkManager::gSyncMutex );
		while(mNetworkConnections->isRunning() &&
			!glfwGetKey( GLFW_KEY_ESC ) &&
			glfwGetWindowParam( GLFW_OPENED ) &&
			!mTerminate)
		{
			sgct::MessageHandler::Instance()->print(".");

			if(mNetworkConnections->areAllNodesConnected())
				break;

			glfwWaitCond( NetworkManager::gCond,
				NetworkManager::gSyncMutex,
				0.1 ); //wait maximum 0.1 sec per iteration

			// Swap front and back rendering buffers
			glfwSwapBuffers();
		}
		glfwUnlockMutex( NetworkManager::gSyncMutex );

		sgct::MessageHandler::Instance()->print("\n");
	}
	else
		sgct::MessageHandler::Instance()->print("Swapgroups (swap-lock) are disabled.\n");
}

void sgct::Engine::calculateFrustums()
{
	SGCTNode * tmpNode = ClusterManager::Instance()->getThisNodePtr();

	for(unsigned int i=0; i<tmpNode->getNumberOfViewports(); i++)
		if( !tmpNode->getViewport(i)->isTracked() ) //if not tracked update, otherwise this is done on the fly
		{
			User * usrPtr = ClusterManager::Instance()->getUserPtr();
			tmpNode->getViewport(i)->calculateFrustum(
				Frustum::Mono,
				usrPtr->getPosPtr(),
				mNearClippingPlaneDist,
				mFarClippingPlaneDist);

			tmpNode->getViewport(i)->calculateFrustum(
				Frustum::StereoLeftEye,
				usrPtr->getPosPtr(Frustum::StereoLeftEye),
				mNearClippingPlaneDist,
				mFarClippingPlaneDist);

			tmpNode->getViewport(i)->calculateFrustum(
				Frustum::StereoRightEye,
				usrPtr->getPosPtr(Frustum::StereoRightEye),
				mNearClippingPlaneDist,
				mFarClippingPlaneDist);
		}
}

void sgct::Engine::parseArguments( int& argc, char**& argv )
{
	//parse arguments
	sgct::MessageHandler::Instance()->print("Parsing arguments...");
	int i=0;
    std::deque<int> argumentsToRemove;
	while( i<argc )
	{
		if( strcmp(argv[i],"-config") == 0 && argc > (i+1))
		{
			configFilename.assign(argv[i+1]);
            argumentsToRemove.push_back(i);
            argumentsToRemove.push_back(i+1);
			i+=2;
		}
		else if( strcmp(argv[i],"--client") == 0 )
		{
			localRunningMode = NetworkManager::LocalClient;
            argumentsToRemove.push_back(i);
			i++;
		}
		else if( strcmp(argv[i],"--slave") == 0 )
		{
			localRunningMode = NetworkManager::LocalClient;
            argumentsToRemove.push_back(i);
			i++;
		}
		else if( strcmp(argv[i],"-local") == 0 && argc > (i+1) )
		{
			localRunningMode = NetworkManager::LocalServer;
			int tmpi = -1;
			std::stringstream ss( argv[i+1] );
			ss >> tmpi;
			ClusterManager::Instance()->setThisNodeId(tmpi);
            argumentsToRemove.push_back(i);
            argumentsToRemove.push_back(i+1);
			i+=2;
		}
		else if( strcmp(argv[i],"--Ignore-Sync") == 0 )
		{
			mIgnoreSync = true;
			argumentsToRemove.push_back(i);
			i++;
		}
		else if( strcmp(argv[i],"--No-FBO") == 0 )
		{
			mFBOMode = NoFBO;
			argumentsToRemove.push_back(i);
			i++;
		}
		else if( strcmp(argv[i],"--Regular-FBO") == 0 )
		{
			mFBOMode = RegularFBO;
			argumentsToRemove.push_back(i);
			i++;
		}
		else if( strcmp(argv[i],"--MultiSampled-FBO") == 0 )
		{
			mFBOMode = MultiSampledFBO;
			argumentsToRemove.push_back(i);
			i++;
		}
		else
			i++; //iterate
	}

    // remove the arguments that have been processed
    if( argumentsToRemove.size() > 0 )
    {
        int newArgc = argc - argumentsToRemove.size();
        char** newArgv = new char*[newArgc];
        int newIterator = 0;
        for( int oldIterator = 0; oldIterator < argc; ++oldIterator )
        {
            if( !argumentsToRemove.empty() && oldIterator == argumentsToRemove.front())
            {
                argumentsToRemove.pop_front();
            }
            else
            {
                newArgv[newIterator] = argv[oldIterator];
                newIterator++;
            }
        }
        argv = newArgv;
        argc = newArgc;
    }

	sgct::MessageHandler::Instance()->print(" Done\n");
}

void sgct::Engine::setDrawFunction(void(*fnPtr)(void))
{
	mDrawFn = fnPtr;
}

void sgct::Engine::setPreSyncFunction(void(*fnPtr)(void))
{
	mPreSyncFn = fnPtr;
}

void sgct::Engine::setPostSyncPreDrawFunction(void(*fnPtr)(void))
{
	mPostSyncPreDrawFn = fnPtr;
}

void sgct::Engine::setPostDrawFunction(void(*fnPtr)(void))
{
	mPostDrawFn = fnPtr;
}

void sgct::Engine::setInitOGLFunction(void(*fnPtr)(void))
{
	mInitOGLFn = fnPtr;
}

void sgct::Engine::setClearBufferFunction(void(*fnPtr)(void))
{
	mClearBufferFn = fnPtr;
}

void sgct::Engine::setCleanUpFunction( void(*fnPtr)(void) )
{
	mCleanUpFn = fnPtr;
}

void sgct::Engine::setExternalControlCallback(void(*fnPtr)(const char *, int, int))
{
	mNetworkCallbackFn = fnPtr;
}

void sgct::Engine::setKeyboardCallbackFunction( void(*fnPtr)(int,int) )
{
	mKeyboardCallbackFn = fnPtr;
}

void sgct::Engine::setCharCallbackFunction( void(*fnPtr)(int, int) )
{
	mCharCallbackFn = fnPtr;
}

void sgct::Engine::setMouseButtonCallbackFunction( void(*fnPtr)(int, int) )
{
	mMouseButtonCallbackFn = fnPtr;
}

void sgct::Engine::setMousePosCallbackFunction( void(*fnPtr)(int, int) )
{
	mMousePosCallbackFn = fnPtr;
}

void sgct::Engine::setMouseScrollCallbackFunction( void(*fnPtr)(int) )
{
	mMouseWheelCallbackFn = fnPtr;
}

void sgct::Engine::clearBuffer(void)
{
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
}

void sgct::Engine::printNodeInfo(unsigned int nodeId)
{
	sgct::MessageHandler::Instance()->print("This node has index %d.\n", nodeId);
}

void sgct::Engine::enterCurrentViewport()
{
	SGCTNode * tmpNode = ClusterManager::Instance()->getThisNodePtr();

	currentViewportCoords[0] =
		static_cast<int>( tmpNode->getCurrentViewport()->getX() * static_cast<float>(getWindowPtr()->getHResolution()));
	currentViewportCoords[1] =
		static_cast<int>( tmpNode->getCurrentViewport()->getY() * static_cast<float>(getWindowPtr()->getVResolution()));
	currentViewportCoords[2] =
		static_cast<int>( tmpNode->getCurrentViewport()->getXSize() * static_cast<float>(getWindowPtr()->getHResolution()));
	currentViewportCoords[3] =
		static_cast<int>( tmpNode->getCurrentViewport()->getYSize() * static_cast<float>(getWindowPtr()->getVResolution()));

	glViewport( currentViewportCoords[0],
		currentViewportCoords[1],
		currentViewportCoords[2],
		currentViewportCoords[3]);
}

void sgct::Engine::calcFPS(double timestamp)
{
	static double lastTimestamp = glfwGetTime();
	mStatistics.setFrameTime(timestamp - lastTimestamp);
	lastTimestamp = timestamp;
    static double renderedFrames = 0.0;
	static double tmpTime = 0.0;
	renderedFrames += 1.0;
	tmpTime += mStatistics.getFrameTime();
	if( tmpTime >= 1.0 )
	{
		mStatistics.setAvgFPS(renderedFrames / tmpTime);
		renderedFrames = 0.0;
		tmpTime = 0.0;

		//don't set if in full screen
		if(getWindowPtr()->getWindowMode() == GLFW_WINDOW)
			getWindowPtr()->setWindowTitle( getBasicInfo() );
	}
}

const double & sgct::Engine::getDt()
{
	return mStatistics.getFrameTime();
}

const double & sgct::Engine::getDrawTime()
{
	return mStatistics.getDrawTime();
}

const double & sgct::Engine::getSyncTime()
{
	return mStatistics.getSyncTime();
}

void sgct::Engine::setNearAndFarClippingPlanes(float _near, float _far)
{
	mNearClippingPlaneDist = _near;
	mFarClippingPlaneDist = _far;
	calculateFrustums();
}

void sgct::Engine::decodeExternalControl(const char * receivedData, int receivedlength, int clientIndex)
{
	if(mNetworkCallbackFn != NULL && receivedlength > 0)
		mNetworkCallbackFn(receivedData, receivedlength, clientIndex);
}

void sgct::Engine::sendMessageToExternalControl(void * data, int length)
{
	if( mNetworkConnections->getExternalControlPtr() != NULL )
		mNetworkConnections->getExternalControlPtr()->sendData( data, length );
}

void sgct::Engine::sendMessageToExternalControl(const std::string msg)
{
	if( mNetworkConnections->getExternalControlPtr() != NULL )
		mNetworkConnections->getExternalControlPtr()->sendData( (void *)msg.c_str(), msg.size() );
}

void sgct::Engine::setExternalControlBufferSize(unsigned int newSize)
{
	if( mNetworkConnections->getExternalControlPtr() != NULL )
		mNetworkConnections->getExternalControlPtr()->setBufferSize(newSize);
}

const char * sgct::Engine::getBasicInfo()
{
	#if (_MSC_VER >= 1400) //visual studio 2005 or later
	sprintf_s( basicInfo, sizeof(basicInfo), "Node: %s (%s) | fps: %.2f",
		localRunningMode == NetworkManager::NotLocal ? ClusterManager::Instance()->getThisNodePtr()->ip.c_str() : "127.0.0.1",
		mNetworkConnections->isComputerServer() ? "master" : "slave",
		mStatistics.getAvgFPS());
    #else
    sprintf( basicInfo, "Node: %s (%s) | fps: %.2f",
		localRunningMode == NetworkManager::NotLocal ? ClusterManager::Instance()->getThisNodePtr()->ip.c_str() : "127.0.0.1",
		mNetworkConnections->isComputerServer() ? "master" : "slave",
		mStatistics.getAvgFPS());
    #endif

	return basicInfo;
}

int sgct::Engine::getKey( const int &key )
{
	return glfwGetKey(key);
}

int sgct::Engine::getMouseButton( const int &button )
{
	return glfwGetMouseButton(button);
}

void sgct::Engine::getMousePos( int * xPos, int * yPos )
{
	glfwGetMousePos(xPos, yPos);
}

void sgct::Engine::setMousePos( const int &xPos, const int &yPos )
{
	glfwSetMousePos(xPos, yPos);
}

int sgct::Engine::getMouseWheel()
{
	return glfwGetMouseWheel();
}

void sgct::Engine::setMouseWheel( const int &pos )
{
	glfwSetMouseWheel(pos);
}

void sgct::Engine::setMousePointerVisibility( bool state )
{
	state ? glfwEnable( GLFW_MOUSE_CURSOR ) : glfwDisable( GLFW_MOUSE_CURSOR );
}

int sgct::Engine::getJoystickParam( const int &joystick, const int &param )
{
	return glfwGetJoystickParam(joystick,param);
}

int sgct::Engine::getJoystickAxes( const int &joystick, float * values, const int &numOfValues)
{
	return glfwGetJoystickPos( joystick, values, numOfValues );
}

int sgct::Engine::getJoystickButtons( const int &joystick, unsigned char * values, const int &numOfValues)
{
	return glfwGetJoystickButtons( joystick, values, numOfValues );
}

size_t sgct::Engine::createTimer( double millisec, void(*fnPtr)(size_t) )
{
    if ( isMaster() )
    {
        // construct the timer object
        TimerInformation timer;
        timer.mCallback = fnPtr;
        timer.mInterval = millisec / 1000.0; // we want to present timers in millisec, but glfwGetTime uses seconds
        timer.mId = mTimerID++;  // use and post-increase
        timer.mLastFired = getTime();
        mTimers.push_back( timer );
        return timer.mId;
    }
    else
		return std::numeric_limits<size_t>::max();
}

void sgct::Engine::stopTimer( size_t id )
{
    if ( isMaster() )
    {
        // iterate over all timers and search for the id
        for( size_t i = 0; i < mTimers.size(); ++i )
        {
            const TimerInformation& currentTimer = mTimers[i];
            if( currentTimer.mId == id )
            {
                mTimers[i].mCallback = NULL;
				// if the id found, delete this timer and return immediately
                mTimers.erase( mTimers.begin() + i );
                return;
            }
        }

        // if we get this far, the searched ID did not exist
        sgct::MessageHandler::Instance()->print("There was no timer with id: %i", id);
    }
}

double sgct::Engine::getTime()
{
	return glfwGetTime();
}

GLFWmutex sgct::Engine::createMutex()
{
    return glfwCreateMutex();
}

GLFWcond sgct::Engine::createCondition()
{
    return glfwCreateCond();
}

void sgct::Engine::destroyCond(GLFWcond &cond)
{
    glfwDestroyCond(cond);
}

void sgct::Engine::destroyMutex(GLFWmutex &mutex)
{
    glfwDestroyMutex(mutex);
}

void sgct::Engine::lockMutex(GLFWmutex &mutex)
{
#ifdef __SGCT_MUTEX_DEBUG__
    fprintf(stderr, "Locking mutex...\n");
#endif
    if(mutex != NULL)
		glfwLockMutex(mutex);
#ifdef __SGCT_MUTEX_DEBUG__
    fprintf(stderr, "Done\n");
#endif
}

void sgct::Engine::unlockMutex(GLFWmutex &mutex)
{
#ifdef __SGCT_MUTEX_DEBUG__
    fprintf(stderr, "Unlocking mutex...\n");
#endif
	if(mutex != NULL)
		glfwUnlockMutex(mutex);
#ifdef __SGCT_MUTEX_DEBUG__
    fprintf(stderr, "Done\n");
#endif
}

void sgct::Engine::waitCond(GLFWcond &cond, GLFWmutex &mutex, double timeout)
{
    glfwWaitCond(cond, mutex, timeout);
}

void sgct::Engine::signalCond(GLFWcond &cond)
{
    glfwSignalCond(cond);
}
