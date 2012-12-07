#include "cinder/app/AppBasic.h"
#include "cinder/audio/Output.h"
#include "cinder/audio/Io.h"

#include "Resources.h"

using namespace ci;
using namespace ci::app;

class AudioPlaybackApp : public AppBasic {
 public:
	void setup();
	void mouseDown( MouseEvent );
	void draw();

private:
	void loadAudio(const std::string& filename)
	{
		try
		{
			audio::SourceRef src = audio::load( loadAsset( filename ) );
			if ( src )
				mAudioSources.push_back( src );
		}
		catch ( Exception& e )
		{
			console() << e.what() << std::endl;
		}
	}
	std::vector<audio::SourceRef> mAudioSources;
};

void AudioPlaybackApp::setup()
{
	loadAudio( "getout.ogg" );
	loadAudio( "ophelia.mp3" );
}

void AudioPlaybackApp::mouseDown( MouseEvent event )
{
	audio::Output::play( mAudioSources.front() );
}

void AudioPlaybackApp::draw()
{
	glClearColor( 0.1f, 0.1f, 0.1f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT );
}

CINDER_APP_BASIC( AudioPlaybackApp, RendererGl )
