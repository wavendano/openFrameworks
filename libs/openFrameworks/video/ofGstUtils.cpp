#include "ofGstUtils.h"
#ifndef TARGET_ANDROID
#include "ofUtils.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <glib-object.h>
#include <glib.h>
#include <algorithm>



void ofGstUtils::startGstMainLoop(){
	static bool initialized = false;
	if(!initialized){
		g_main_loop_new (NULL, FALSE);
		initialized=true;
	}
}




//-------------------------------------------------
//----------------------------------------- gstUtils
//-------------------------------------------------

static bool plugin_registered = false;
static bool gst_inited = false;


// called when the appsink notifies us that there is a new buffer ready for
// processing

static GstFlowReturn on_new_buffer_from_source (GstAppSink * elt, void * data){
	GstBuffer *buffer = gst_app_sink_pull_buffer (GST_APP_SINK (elt));
	return ((ofGstUtils*)data)->buffer_cb(buffer);
}

static GstFlowReturn on_new_preroll_from_source (GstAppSink * elt, void * data){
	GstBuffer *buffer = gst_app_sink_pull_preroll(GST_APP_SINK (elt));
	return ((ofGstUtils*)data)->preroll_cb(buffer);
}

static void on_eos_from_source (GstAppSink * elt, void * data){
	((ofGstUtils*)data)->eos_cb();
}

static gboolean appsink_plugin_init (GstPlugin * plugin)
{
  gst_element_register (plugin, "appsink", GST_RANK_NONE, GST_TYPE_APP_SINK);

  return TRUE;
}



ofGstUtils::ofGstUtils() {
	bLoaded 					= false;
	speed 						= 1;
	bPaused						= false;
	bIsMovieDone				= false;
	bPlaying					= false;
	loopMode					= OF_LOOP_NONE;
	bFrameByFrame 				= false;

	gstPipeline					= NULL;
	gstSink						= NULL;

	durationNanos				= 0;

	isAppSink					= false;
	isStream					= false;

	appsink						= NULL;

	if(!g_thread_supported()){
		g_thread_init(NULL);
	}
	if(!gst_inited){
		gst_init (NULL, NULL);
		gst_inited=true;
		ofLog(OF_LOG_VERBOSE,"ofGstUtils: gstreamer inited");
	}
	if(!plugin_registered){
		gst_plugin_register_static(GST_VERSION_MAJOR, GST_VERSION_MINOR,
					"appsink", (char*)"Element application sink",
					appsink_plugin_init, "0.1", "LGPL", "ofVideoPlayer", "openFrameworks",
					"http://openframeworks.cc/");
		plugin_registered=true;
	}

}

ofGstUtils::~ofGstUtils() {
	close();
}

GstFlowReturn ofGstUtils::preroll_cb(GstBuffer * buffer){
	bIsMovieDone = false;
	if(appsink) return appsink->on_preroll(buffer);
	else return GST_FLOW_OK;
}

GstFlowReturn ofGstUtils::buffer_cb(GstBuffer * buffer){
	bIsMovieDone = false;
	if(appsink) return appsink->on_buffer(buffer);
	else return GST_FLOW_OK;
}

void ofGstUtils::eos_cb(){
	bIsMovieDone = true;
	if(appsink) appsink->on_eos();
}

bool ofGstUtils::setPipelineWithSink(string pipeline, string sinkname, bool isStream){
	ofGstUtils::startGstMainLoop();

	gchar* pipeline_string =
		g_strdup((pipeline).c_str());

	GError * error = NULL;
	gstPipeline = gst_parse_launch (pipeline_string, &error);

	ofLog(OF_LOG_NOTICE, "gstreamer pipeline: %s", pipeline_string);
	if(error!=NULL){
		ofLog(OF_LOG_ERROR,"couldnt create pipeline: " + string(error->message));
		return false;
	}

	gstSink = gst_bin_get_by_name(GST_BIN(gstPipeline),sinkname.c_str());

	if(!gstSink){
		ofLogError() << "couldn't get sink from string pipeline";
	}

	return setPipelineWithSink(gstPipeline,gstSink,isStream);
}

bool ofGstUtils::setPipelineWithSink(GstElement * pipeline, GstElement * sink, bool isStream_){
	ofGstUtils::startGstMainLoop();

	gstPipeline = pipeline;
	gstSink = sink;
	isStream = isStream_;

	if(gstSink){
		gst_base_sink_set_sync(GST_BASE_SINK(gstSink), true);
	}

	if(gstSink && string(gst_plugin_feature_get_name( GST_PLUGIN_FEATURE(gst_element_get_factory(gstSink))))=="appsink"){
		isAppSink = true;
	}else{
		isAppSink = false;
	}

	return startPipeline();
}

void ofGstUtils::setFrameByFrame(bool _bFrameByFrame){
	bFrameByFrame = _bFrameByFrame;
	if(gstSink){
		g_object_set (G_OBJECT (gstSink), "sync", !bFrameByFrame, (void*)NULL);
	}
}

bool ofGstUtils::isFrameByFrame(){
	return bFrameByFrame;
}

bool ofGstUtils::startPipeline(){

	bPaused 			= true;
	speed 				= 1.0f;


	if(gst_element_set_state (GST_ELEMENT(gstPipeline), GST_STATE_READY) ==	GST_STATE_CHANGE_FAILURE) {
		ofLog(OF_LOG_ERROR, "GStreamer: unable to set pipeline to ready\n");

		return false;
	}
	if(gst_element_get_state (GST_ELEMENT(gstPipeline), NULL, NULL, 10 * GST_SECOND)==GST_STATE_CHANGE_FAILURE){
		ofLog(OF_LOG_ERROR, "GStreamer: unable to get pipeline to ready\n");
		return false;
	}

	// pause the pipeline
	if(gst_element_set_state(GST_ELEMENT(gstPipeline), GST_STATE_PAUSED) ==	GST_STATE_CHANGE_FAILURE) {
		ofLog(OF_LOG_ERROR, "GStreamer: unable to set pipeline to paused\n");

		return false;
	}

	// wait for paused state to query the duration
	if(!isStream){
		GstState state = GST_STATE_PAUSED;
		if(gst_element_get_state(gstPipeline,&state,NULL,2*GST_SECOND)==GST_STATE_CHANGE_FAILURE){
			ofLog(OF_LOG_ERROR, "GStreamer: unable to get pipeline to paused\n");
			return false;
		}
		bPlaying = true;
		bLoaded = true;
	}



	if(isAppSink){
		ofLogVerbose() << "attaching callbacks";
		// set the appsink to not emit signals, we are using callbacks instead
		// and frameByFrame to get buffers by polling instead of callback
		g_object_set (G_OBJECT (gstSink), "emit-signals", FALSE, "sync", !bFrameByFrame, (void*)NULL);

		if(!bFrameByFrame){
			GstAppSinkCallbacks gstCallbacks;
			gstCallbacks.eos = &on_eos_from_source;
			gstCallbacks.new_preroll = &on_new_preroll_from_source;
			gstCallbacks.new_buffer = &on_new_buffer_from_source;

			gst_app_sink_set_callbacks(GST_APP_SINK(gstSink), &gstCallbacks, this, NULL);
		}

	}

	if(!isStream){
		setSpeed(1.0);
	}

	ofAddListener(ofEvents().update,this,&ofGstUtils::update);

	return true;
}

void ofGstUtils::play(){
	setPaused(false);

	//this is if we set the speed first but it only can be set when we are playing.
	if(!isStream) setSpeed(speed);
}

void ofGstUtils::setPaused(bool _bPause){
	bPaused = _bPause;
	//timeLastIdle = ofGetElapsedTimeMillis();
	if(bLoaded){
		if(bPlaying){
			if(bPaused){
				gst_element_set_state (gstPipeline, GST_STATE_PAUSED);
			}else{
				gst_element_set_state (gstPipeline, GST_STATE_PLAYING);
			}
		}else{
			GstState state = GST_STATE_PAUSED;
			gst_element_set_state (gstPipeline, state);
			gst_element_get_state(gstPipeline,&state,NULL,2*GST_SECOND);
			if(!bPaused){
				gst_element_set_state (gstPipeline, GST_STATE_PLAYING);
			}
			bPlaying = true;
		}
	}
}

void ofGstUtils::stop(){
	if(!bPlaying) return;
	GstState state = GST_STATE_PAUSED;
	if(!bPaused){
		gst_element_set_state (gstPipeline, state);
		gst_element_get_state(gstPipeline,&state,NULL,2*GST_SECOND);
	}
	state = GST_STATE_READY;
	gst_element_set_state (gstPipeline, state);
	gst_element_get_state(gstPipeline,&state,NULL,2*GST_SECOND);
	bPlaying = false;
	bPaused = true;
}

float ofGstUtils::getPosition(){
	if(gstPipeline){
		gint64 pos=0;
		GstFormat format=GST_FORMAT_TIME;
		if(!gst_element_query_position(GST_ELEMENT(gstPipeline),&format,&pos)){
			ofLog(OF_LOG_VERBOSE,"GStreamer: cannot query position");
			return -1;
		}
		return (float)pos/(float)durationNanos;
	}else{
		return -1;
	}
}

float ofGstUtils::getSpeed(){
	return speed;
}

float ofGstUtils::getDuration(){
	return (float)getDurationNanos()/(float)GST_SECOND;
}

int64_t ofGstUtils::getDurationNanos(){
	GstFormat format = GST_FORMAT_TIME;

	if(!gst_element_query_duration(getPipeline(),&format,&durationNanos))
		ofLog(OF_LOG_WARNING,"GStreamer: cannot query time duration");

	return durationNanos;

}

bool  ofGstUtils::getIsMovieDone(){
	if(isAppSink){
		return gst_app_sink_is_eos(GST_APP_SINK(gstSink));
	}else{
		return bIsMovieDone;
	}
}

void ofGstUtils::setPosition(float pct){
	//pct = CLAMP(pct, 0,1);// check between 0 and 1;
	GstFormat format = GST_FORMAT_TIME;
	GstSeekFlags flags = (GstSeekFlags) (GST_SEEK_FLAG_ACCURATE | GST_SEEK_FLAG_FLUSH);
	gint64 pos = (guint64)((double)pct*(double)durationNanos);

	/*if(bPaused){
		seek_lock();
		gst_element_set_state (gstPipeline, GST_STATE_PLAYING);
		posChangingPaused=true;
		seek_unlock();
	}*/
	if(speed>0){
		if(!gst_element_seek(GST_ELEMENT(gstPipeline),speed, 	format,
				flags,
				GST_SEEK_TYPE_SET,
				pos,
				GST_SEEK_TYPE_SET,
				-1)) {
		ofLog(OF_LOG_WARNING,"GStreamer: unable to seek");
		}
	}else{
		if(!gst_element_seek(GST_ELEMENT(gstPipeline),speed, 	format,
				flags,
				GST_SEEK_TYPE_SET,
				0,
				GST_SEEK_TYPE_SET,
				pos)) {
		ofLog(OF_LOG_WARNING,"GStreamer: unable to seek");
		}
	}
}

void ofGstUtils::setVolume(float volume){
	gdouble gvolume = volume;
	g_object_set(G_OBJECT(gstPipeline), "volume", gvolume, (void*)NULL);
}

void ofGstUtils::setLoopState(ofLoopType state){
	loopMode = state;
}

void ofGstUtils::setSpeed(float _speed){
	GstFormat format = GST_FORMAT_TIME;
	GstSeekFlags flags = (GstSeekFlags) (GST_SEEK_FLAG_SKIP | GST_SEEK_FLAG_ACCURATE | GST_SEEK_FLAG_FLUSH);
	gint64 pos;

	if(_speed==0){
		gst_element_set_state (gstPipeline, GST_STATE_PAUSED);
		return;
	}

	if(!gst_element_query_position(GST_ELEMENT(gstPipeline),&format,&pos) || pos<0){
		//ofLog(OF_LOG_ERROR,"GStreamer: cannot query position");
		return;
	}

	speed = _speed;
	//pos = (float)gstData.lastFrame * (float)fps_d / (float)fps_n * GST_SECOND;

	if(!bPaused)
		gst_element_set_state (gstPipeline, GST_STATE_PLAYING);

	if(speed>0){
		if(!gst_element_seek(GST_ELEMENT(gstPipeline),speed, 	format,
				flags,
				GST_SEEK_TYPE_SET,
				pos,
				GST_SEEK_TYPE_SET,
				-1)) {
		ofLog(OF_LOG_WARNING,"GStreamer: unable to change speed");
		}
	}else{
		if(!gst_element_seek(GST_ELEMENT(gstPipeline),speed, 	format,
				flags,
				GST_SEEK_TYPE_SET,
				0,
				GST_SEEK_TYPE_SET,
				pos)) {
		ofLog(OF_LOG_WARNING,"GStreamer: unable to change speed");
		}
	}

	ofLog(OF_LOG_VERBOSE,"Gstreamer: speed change to %f", speed);

}

void ofGstUtils::close(){
	if(bPlaying){
		stop();
	}
	if(bLoaded){
		gst_element_set_state(GST_ELEMENT(gstPipeline), GST_STATE_NULL);
		gst_element_get_state(gstPipeline,NULL,NULL,2*GST_SECOND);
		// gst_object_unref(gstSink); this crashes, why??

		ofEventArgs args;
		update(args);

		gst_object_unref(gstPipeline);
		gstPipeline = NULL;
		gstSink = NULL;
	}

	bLoaded = false;
	ofRemoveListener(ofEvents().update,this,&ofGstUtils::update);
}

static string getName(GstState state){
	switch(state){
	case   GST_STATE_VOID_PENDING:
		return "void pending";
	case   GST_STATE_NULL:
		return "null";
	case   GST_STATE_READY:
		return "ready";
	case   GST_STATE_PAUSED:
		return "paused";
	case   GST_STATE_PLAYING:
		return "playing";
	default:
		return "";
	}
}

void ofGstUtils::update(ofEventArgs & args){
	gstHandleMessage();
}

void ofGstUtils::gstHandleMessage(){
	GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(gstPipeline));
	while(gst_bus_have_pending(bus)) {
		GstMessage* msg = gst_bus_pop(bus);
		if(appsink && appsink->on_message(msg)) continue;

		ofLogVerbose() << "GStreamer: Got " << GST_MESSAGE_TYPE_NAME(msg) << " message from " << GST_MESSAGE_SRC_NAME(msg);

		switch (GST_MESSAGE_TYPE (msg)) {

			case GST_MESSAGE_BUFFERING:
				gint pctBuffered;
				gst_message_parse_buffering(msg,&pctBuffered);
				ofLog(OF_LOG_VERBOSE,"GStreamer: buffering %i\%", pctBuffered);
				/*if(pctBuffered<100){
					gst_element_set_state (gstPipeline, GST_STATE_PAUSED);
				}else if(!bPaused){
					gst_element_set_state (gstPipeline, GST_STATE_PLAYING);
				}*/
			break;

			case GST_MESSAGE_DURATION:{
				GstFormat format=GST_FORMAT_TIME;
				gst_element_query_duration(gstPipeline,&format,&durationNanos);
			}break;

			case GST_MESSAGE_STATE_CHANGED:{
				GstState oldstate, newstate, pendstate;
				gst_message_parse_state_changed(msg, &oldstate, &newstate, &pendstate);
				if(isStream && newstate==GST_STATE_PAUSED && !bPlaying ){
					bLoaded = true;
					bPlaying = true;
					if(!bPaused){
						cout << "setting stream pipeline to play " << endl;
						play();
					}
				}
				ofLogVerbose() << "GStreamer: " << GST_MESSAGE_SRC_NAME(msg) << " state changed from " << getName(oldstate) + " to " + getName(newstate) + " (" + getName(pendstate) + ")";
			}break;

			case GST_MESSAGE_ASYNC_DONE:
				ofLog(OF_LOG_VERBOSE,"GStreamer: async done");
			break;

			case GST_MESSAGE_ERROR: {
				GError *err;
				gchar *debug;
				gst_message_parse_error(msg, &err, &debug);

				ofLog(OF_LOG_ERROR, "GStreamer Plugin: Embedded video playback halted; module %s reported: %s",
					  gst_element_get_name(GST_MESSAGE_SRC (msg)), err->message);

				g_error_free(err);
				g_free(debug);

				gst_element_set_state(GST_ELEMENT(gstPipeline), GST_STATE_NULL);

			}break;

			case GST_MESSAGE_EOS:
				ofLog(OF_LOG_VERBOSE,"GStreamer: end of the stream.");
				bIsMovieDone = true;
				
				if(appsink && !isAppSink) appsink->on_eos();

				switch(loopMode){

					case OF_LOOP_NORMAL:{
						GstFormat format = GST_FORMAT_TIME;
						GstSeekFlags flags = (GstSeekFlags) (GST_SEEK_FLAG_FLUSH |GST_SEEK_FLAG_KEY_UNIT);
						gint64 pos;
						gst_element_query_position(GST_ELEMENT(gstPipeline),&format,&pos);

						if(!gst_element_seek(GST_ELEMENT(gstPipeline),
											speed,
											format,
											flags,
											GST_SEEK_TYPE_SET,
											0,
											GST_SEEK_TYPE_SET,
											durationNanos)) {
							ofLog(OF_LOG_WARNING,"GStreamer: unable to seek");
						}
					}break;

					case OF_LOOP_PALINDROME:{
						GstFormat format = GST_FORMAT_TIME;
						GstSeekFlags flags = (GstSeekFlags) (GST_SEEK_FLAG_FLUSH |GST_SEEK_FLAG_KEY_UNIT);
						gint64 pos;
						gst_element_query_position(GST_ELEMENT(gstPipeline),&format,&pos);
						float loopSpeed;
						if(pos>0)
							loopSpeed=-speed;
						else
							loopSpeed=speed;
						if(!gst_element_seek(GST_ELEMENT(gstPipeline),
											loopSpeed,
											GST_FORMAT_UNDEFINED,
											flags,
											GST_SEEK_TYPE_NONE,
											0,
											GST_SEEK_TYPE_NONE,
											0)) {
							ofLog(OF_LOG_WARNING,"GStreamer: unable to seek");
						}
					}break;

					default:
					break;
				}

			break;

			default:
				ofLogVerbose() << "GStreamer: unhandled message from " << GST_MESSAGE_SRC_NAME(msg);
			break;
		}
		gst_message_unref(msg);
	}

	gst_object_unref(GST_OBJECT(bus));
}

GstElement 	* ofGstUtils::getPipeline(){
	return gstPipeline;
}

GstElement 	* ofGstUtils::getSink(){
	return gstSink;
}


void ofGstUtils::setSinkListener(ofGstAppSink * appsink_){
	appsink = appsink_;
}

unsigned long ofGstUtils::getMinLatencyNanos(){
	GstClockTime minlat, maxlat;
	GstQuery * q = gst_query_new_latency();
	if (gst_element_query (gstPipeline, q)) {
		 gboolean live;
		 gst_query_parse_latency (q, &live, &minlat, &maxlat);
	}
	gst_query_unref (q);
	return minlat;
}

unsigned long ofGstUtils::getMaxLatencyNanos(){
	GstClockTime minlat, maxlat;
	GstQuery * q = gst_query_new_latency();
	if (gst_element_query (gstPipeline, q)) {
		 gboolean live;
		 gst_query_parse_latency (q, &live, &minlat, &maxlat);
	}
	gst_query_unref (q);
	return maxlat;
}



//-------------------------------------------------
//----------------------------------------- videoUtils
//-------------------------------------------------



ofGstVideoUtils::ofGstVideoUtils(){
	bIsFrameNew					= false;
	bHavePixelsChanged			= false;
	bBackPixelsChanged			= false;
	buffer = 0;
	prevBuffer = 0;
}

ofGstVideoUtils::~ofGstVideoUtils(){
	close();
}

void ofGstVideoUtils::close(){
	ofGstUtils::close();
	Poco::ScopedLock<ofMutex> lock(mutex);
	pixels.clear();
	backPixels.clear();
	bIsFrameNew					= false;
	bHavePixelsChanged			= false;
	bBackPixelsChanged			= false;
	if(prevBuffer) gst_buffer_unref (prevBuffer);
	if(buffer) gst_buffer_unref (buffer);
	prevBuffer = 0;
	buffer = 0;
}

bool ofGstVideoUtils::isFrameNew(){
	return bIsFrameNew;
}

unsigned char * ofGstVideoUtils::getPixels(){
	return pixels.getPixels();
}

ofPixelsRef ofGstVideoUtils::getPixelsRef(){
	return pixels;
}

void ofGstVideoUtils::update(){
	if (isLoaded()){
		if(!isFrameByFrame()){
			mutex.lock();
				bHavePixelsChanged = bBackPixelsChanged;
				if (bHavePixelsChanged){
					bBackPixelsChanged=false;
					pixels.swap(backPixels);
					if(prevBuffer) gst_buffer_unref (prevBuffer);
					prevBuffer = buffer;
				}

			mutex.unlock();
		}else{
			GstBuffer *buffer;

			//get the buffer from appsink
			if(isPaused()) buffer = gst_app_sink_pull_preroll (GST_APP_SINK (getSink()));
			else buffer = gst_app_sink_pull_buffer (GST_APP_SINK (getSink()));

			if(buffer){
				if(pixels.isAllocated()){
					if(prevBuffer) gst_buffer_unref (prevBuffer);
					//memcpy (pixels.getPixels(), GST_BUFFER_DATA (buffer), size);
					pixels.setFromExternalPixels(GST_BUFFER_DATA (buffer),pixels.getWidth(),pixels.getHeight(),pixels.getNumChannels());
					prevBuffer = buffer;
					bHavePixelsChanged=true;
				}
			}
		}
	}else{
		ofLog(OF_LOG_WARNING,"ofGstVideoUtils not loaded");
	}
	bIsFrameNew = bHavePixelsChanged;
	bHavePixelsChanged = false;
}

float ofGstVideoUtils::getHeight(){
	return pixels.getHeight();
}

float ofGstVideoUtils::getWidth(){
	return pixels.getWidth();
}

bool ofGstVideoUtils::setPipeline(string pipeline, int bpp, bool isStream, int w, int h){
	string caps;
	if(bpp==8)
		caps="video/x-raw-gray, depth=8, bpp=8";
	else if(bpp==32)
		caps="video/x-raw-rgb, depth=24, bpp=32, endianness=4321, red_mask=0xff0000, green_mask=0x00ff00, blue_mask=0x0000ff, alpha_mask=0x000000ff";
	else
		caps="video/x-raw-rgb, depth=24, bpp=24, endianness=4321, red_mask=0xff0000, green_mask=0x00ff00, blue_mask=0x0000ff, alpha_mask=0x000000ff";

	if(w!=-1 && h!=-1){
		caps+=", width=" + ofToString(w) + ", height=" + ofToString(h);
	}

	gchar* pipeline_string =
		g_strdup((pipeline + " ! appsink name=ofappsink caps=\"" + caps + "\"").c_str()); // caps=video/x-raw-rgb

	if((isStream && (w==-1 || h==-1)) || allocate(w,h,bpp)){
		return setPipelineWithSink(pipeline_string,"ofappsink",isStream);
	}else{
		return false;
	}
}

bool ofGstVideoUtils::allocate(int w, int h, int _bpp){
	pixels.allocate(w,h,_bpp/8);
	backPixels.allocate(w,h,_bpp/8);
	prevBuffer = 0;
	pixels.set(0);

	bHavePixelsChanged = pixels.isAllocated();
	return pixels.isAllocated();
}


GstFlowReturn ofGstVideoUtils::preroll_cb(GstBuffer * _buffer){
	guint size;

	size = GST_BUFFER_SIZE (_buffer);
	if(pixels.isAllocated() && pixels.getWidth()*pixels.getHeight()*pixels.getBytesPerPixel()!=(int)size){
		ofLog(OF_LOG_ERROR, "on_preproll: error preroll buffer size: " + ofToString(size) + "!= init size: " + ofToString(pixels.getWidth()*pixels.getHeight()*pixels.getBytesPerPixel()));
		gst_buffer_unref (_buffer);
		return GST_FLOW_ERROR;
	}
	mutex.lock();
	if(bBackPixelsChanged && buffer) gst_buffer_unref (buffer);
	if(pixels.isAllocated()){
		buffer = _buffer;
		backPixels.setFromExternalPixels(GST_BUFFER_DATA (buffer),pixels.getWidth(),pixels.getHeight(),pixels.getNumChannels());
		eventPixels.setFromExternalPixels(GST_BUFFER_DATA (buffer),pixels.getWidth(),pixels.getHeight(),pixels.getNumChannels());
		bBackPixelsChanged=true;
		mutex.unlock();
		ofNotifyEvent(prerollEvent,eventPixels);
	}else{
		if(isStream && appsink){
			appsink->on_stream_prepared();
		}else{
			ofLog(OF_LOG_WARNING,"received a preroll without allocation");
		}
		mutex.unlock();
	}
	return ofGstUtils::preroll_cb(_buffer);
}

GstFlowReturn ofGstVideoUtils::buffer_cb(GstBuffer * _buffer){

	guint size;

	size = GST_BUFFER_SIZE (_buffer);
	if(pixels.isAllocated() && pixels.getWidth()*pixels.getHeight()*pixels.getBytesPerPixel()!=(int)size){
		ofLog(OF_LOG_ERROR, "on_preproll: error on new buffer, buffer size: " + ofToString(size) + "!= init size: " + ofToString(pixels.getWidth()*pixels.getHeight()*pixels.getBytesPerPixel()));
		gst_buffer_unref (_buffer);
		return GST_FLOW_ERROR;
	}
	mutex.lock();
	if(bBackPixelsChanged && buffer) gst_buffer_unref (buffer);
	if(pixels.isAllocated()){
		buffer = _buffer;
		backPixels.setFromExternalPixels(GST_BUFFER_DATA (buffer),pixels.getWidth(),pixels.getHeight(),pixels.getNumChannels());
		eventPixels.setFromExternalPixels(GST_BUFFER_DATA (buffer),pixels.getWidth(),pixels.getHeight(),pixels.getNumChannels());
		bBackPixelsChanged=true;
		mutex.unlock();
		ofNotifyEvent(bufferEvent,eventPixels);
	}else{
		if(isStream && appsink){
			appsink->on_stream_prepared();
		}else{
			ofLog(OF_LOG_WARNING,"received a preroll without allocation");
		}
		mutex.unlock();
	}

	return ofGstUtils::buffer_cb(buffer);
}

void ofGstVideoUtils::eos_cb(){
	ofGstUtils::eos_cb();
	ofEventArgs args;
	ofNotifyEvent(eosEvent,args);
}

#endif
