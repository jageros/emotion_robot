/*
* 语音听写(iFly Auto Transform)技术能够实时地将语音转换成对应的文字。
*/
#include<ros/ros.h>
#include<std_msgs/Int32.h>
#include<std_msgs/String.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "qisr.h"
#include "msp_cmn.h"
#include "msp_errors.h"
#include "speech_recognizer.h"

#define FRAME_LEN	640 
#define	BUFFER_SIZE	4096

#define ASRCMD		1

using namespace std;

string result;
bool pubFlag = false;
bool recFlag = true;


/* Upload User words */
static int upload_userwords()
{
	char*			userwords	=	NULL;
	size_t			len			=	0;
	size_t			read_len	=	0;
	FILE*			fp			=	NULL;
	int				ret			=	-1;

	fp = fopen("src/voice_system/userwords.txt", "rb");
	if (NULL == fp)										
	{
		printf("\nopen [userwords.txt] failed! \n");
		goto upload_exit;
	}

	fseek(fp, 0, SEEK_END);
	len = ftell(fp); 
	fseek(fp, 0, SEEK_SET);  					
	
	userwords = (char*)malloc(len + 1);
	if (NULL == userwords)
	{
		printf("\nout of memory! \n");
		goto upload_exit;
	}

	read_len = fread((void*)userwords, 1, len, fp); 
	if (read_len != len)
	{
		printf("\nread [userwords.txt] failed!\n");
		goto upload_exit;
	}
	userwords[len] = '\0';
	
	MSPUploadData("userwords", userwords, len, "sub = uup, dtt = userword", &ret); 
	if (MSP_SUCCESS != ret)
	{
		printf("\nMSPUploadData failed ! errorCode: %d \n", ret);
		goto upload_exit;
	}
	
upload_exit:
	if (NULL != fp)
	{
		fclose(fp);
		fp = NULL;
	}	
	if (NULL != userwords)
	{
		free(userwords);
		userwords = NULL;
	}
	
	return ret;
}


static void show_result(char *str, char is_over)
{
	printf("\rResult: [ %s ]", str);
	if(is_over)
		putchar('\n');

	string s(str);
	result = s;
	pubFlag = true;//设置 flag 使其能发布消息去 nlu topic
}

static char *g_result = NULL;
static unsigned int g_buffersize = BUFFER_SIZE;

void on_result(const char *result, char is_last)
{
	if (result) {
		size_t left = g_buffersize - 1 - strlen(g_result);
		size_t size = strlen(result);
		if (left < size) {
			g_result = (char*)realloc(g_result, g_buffersize + BUFFER_SIZE);
			if (g_result)
				g_buffersize += BUFFER_SIZE;
			else {
				printf("mem alloc failed\n");
				return;
			}
		}
		strncat(g_result, result, size);
		show_result(g_result, is_last);
	}
}
void on_speech_begin()
{
	if (g_result)
	{
		free(g_result);
	}
	g_result = (char*)malloc(BUFFER_SIZE);
	g_buffersize = BUFFER_SIZE;
	memset(g_result, 0, g_buffersize);

	printf("Start Listening...\n");
}
void on_speech_end(int reason)
{
	if (reason == END_REASON_VAD_DETECT)
	{
		printf("\nSpeaking done \n");

		recFlag = false;//当检测到断点
	}	
	else
		printf("\nRecognizer error %d\n", reason);
}

/* demo send audio data from a file */
static void demo_file(const char* audio_file, const char* session_begin_params)
{
	int	errcode = 0;
	FILE*	f_pcm = NULL;
	char*	p_pcm = NULL;
	unsigned long	pcm_count = 0;
	unsigned long	pcm_size = 0;
	unsigned long	read_size = 0;
	struct speech_rec iat;
	struct speech_rec_notifier recnotifier = {
		on_result,
		on_speech_begin,
		on_speech_end
	};

	if (NULL == audio_file)
		goto iat_exit;

	f_pcm = fopen(audio_file, "rb");
	if (NULL == f_pcm)
	{
		printf("\nopen [%s] failed! \n", audio_file);
		goto iat_exit;
	}

	fseek(f_pcm, 0, SEEK_END);
	pcm_size = ftell(f_pcm);
	fseek(f_pcm, 0, SEEK_SET);

	p_pcm = (char *)malloc(pcm_size);
	if (NULL == p_pcm)
	{
		printf("\nout of memory! \n");
		goto iat_exit;
	}

	read_size = fread((void *)p_pcm, 1, pcm_size, f_pcm);
	if (read_size != pcm_size)
	{
		printf("\nread [%s] error!\n", audio_file);
		goto iat_exit;
	}

	errcode = sr_init(&iat, session_begin_params, SR_USER, &recnotifier);
	if (errcode) {
		printf("speech recognizer init failed : %d\n", errcode);
		goto iat_exit;
	}

	errcode = sr_start_listening(&iat);
	if (errcode) {
		printf("\nsr_start_listening failed! error code:%d\n", errcode);
		goto iat_exit;
	}

	while (1)
	{
		unsigned int len = 10 * FRAME_LEN; /* 200ms audio */
		int ret = 0;

		if (pcm_size < 2 * len)
			len = pcm_size;
		if (len <= 0)
			break;

		ret = sr_write_audio_data(&iat, &p_pcm[pcm_count], len);

		if (0 != ret)
		{
			printf("\nwrite audio data failed! error code:%d\n", ret);
			goto iat_exit;
		}

		pcm_count += (long)len;
		pcm_size -= (long)len;		
	}

	errcode = sr_stop_listening(&iat);
	if (errcode) {
		printf("\nsr_stop_listening failed! error code:%d \n", errcode);
		goto iat_exit;
	}

iat_exit:
	if (NULL != f_pcm)
	{
		fclose(f_pcm);
		f_pcm = NULL;
	}
	if (NULL != p_pcm)
	{
		free(p_pcm);
		p_pcm = NULL;
	}

	sr_stop_listening(&iat);
	sr_uninit(&iat);
}

/* demo recognize the audio from microphone */
static void demo_mic(const char* session_begin_params)
{
	int errcode;
	int i = 0;

	struct speech_rec iat;

	struct speech_rec_notifier recnotifier = {
		on_result,
		on_speech_begin,
		on_speech_end
	};

	errcode = sr_init(&iat, session_begin_params, SR_MIC, &recnotifier);
	if (errcode) {
		printf("speech recognizer init failed\n");
		return;
	}
	errcode = sr_start_listening(&iat);
	if (errcode) {
		printf("start listen failed %d\n", errcode);
	}
	/* demo 15 seconds recording */
	//	while(i++ < 15)
	//		sleep(1);
	while(recFlag)
	{
		sleep(1);
	}
	errcode = sr_stop_listening(&iat);
	if (errcode) {
		printf("stop listening failed %d\n", errcode);
	}

	sr_uninit(&iat);
}


void asrProcess()
{
	int ret = MSP_SUCCESS;
	//	int upload_on =	1; /* whether upload the user word */
	/* login params, please do keep the appid correct */
	const char* login_params = "appid = 587359f0, work_dir = .";
	//	int aud_src = 0; /* from mic 1 or file 0 */

	/*

	* See "iFlytek MSC Reference Manual"
	*/
	const char* session_begin_params =
		"sub = iat, domain = iat, language = zh_cn, "
		"accent = mandarin, sample_rate = 16000, "
		"result_type = plain, result_encoding = utf8";

	/* Login first. the 1st arg is username, the 2nd arg is password

	 * just set them as NULL. the 3rd arg is login paramertes 
	 * */
	ret = MSPLogin(NULL, NULL, login_params);
	if (MSP_SUCCESS != ret)	{
		printf("MSPLogin failed , Error code %d.\n",ret);
		goto exit; // login fail, exit the program
	}

	//	printf("Want to upload the user words ? \n0: No.\n1: Yes\n");
	//	scanf("%d", &upload_on);
	//	if (upload_on)
	//	{
	//		printf("Uploading the user words ...\n");
	//		ret = upload_userwords();
	//		if (MSP_SUCCESS != ret)
	//			goto exit;	
	//		printf("Uploaded successfully\n");
	//	}

	//	printf("Where the audio comes from?\n"
	//			"0: From a audio file.\n1: From microphone.\n");
	//	scanf("%d", &aud_src);
	//	if(aud_src != 0) {
	//		printf("Demo recognizing the speech from microphone\n");
	//		printf("Speak in 15 seconds\n");

	//		demo_mic(session_begin_params);

	//		printf("15 sec passed\n");
	//	} else {
	//		printf("Demo recgonizing the speech from a recorded audio file\n");
	//		demo_file("wav/iflytek02.wav", session_begin_params); 
	//	}
	
	printf("Uploading the user words ...\n");
	ret = upload_userwords();
	if (MSP_SUCCESS != ret)
		goto exit;	
	printf("Uploaded successfully\n");
	printf("开始语音识别\n");
	demo_mic(session_begin_params);

exit:
	MSPLogout(); // Logout...
}


void asrCallback(const std_msgs::Int32::ConstPtr& in_msg)
{
	//ROS_INFO_STREAM("语音识别功能开启\n");
	std::cout<<"语音识别功能开启"<<endl;
	if(in_msg->data == ASRCMD)
	{
		asrProcess();
	}
}


/* main thread: start/stop record ; query the result of recgonization.
 * record thread: record callback(data write)
 * helper thread: ui(keystroke detection)
 */
int main(int argc, char* argv[])
{
	ros::init(argc, argv, "xf_asr_node");	//创建节点 xf_asr_node
	ros::NodeHandle nd;  				//订阅话题 /voice/xf_asr_topic
	ros::Subscriber sub = nd.subscribe("/voice/xf_asr_topic", 1 , asrCallback);
							//发布消息 /voice/tuling_nlu_topic
	ros::Publisher pub = nd.advertise<std_msgs::String>("/voice/tuling_nlu_topic", 10);
	ros::Rate loop_rate(10);

	while(ros::ok())
	{
		if(pubFlag)
		{
			std_msgs::String out_msg;
			out_msg.data = result;
			pub.publish(out_msg);
			pubFlag = false;//重置
			recFlag = true;
		}
		ros::spinOnce();			//调用一次回调函数

		loop_rate.sleep();	
	}

	return 0;
}