import os
import subprocess
import struct
import shutil
import argparse

def convert_to_gmv(input_video, output_gmv):
    temp_dir = "gmv_temp_frames"
    temp_audio = "gmv_temp_audio.ogg"
    
    if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
        print("ffmpeg or ffprobe not found!")
        return

    if not os.path.exists(input_video):
        print(f"input file '{input_video}' not found!")
        return

    if os.path.exists(temp_dir): shutil.rmtree(temp_dir)
    if os.path.exists(temp_audio): os.remove(temp_audio)
    os.makedirs(temp_dir)

    try:
        # audio check
        print("checking for audio stream...")
        has_audio = False
        audio_check = subprocess.check_output([
            'ffprobe', '-v', 'error', '-select_streams', 'a', 
            '-show_entries', 'stream=codec_type', '-of', 'csv=p=0', input_video
        ]).decode().strip()
        
        if 'audio' in audio_check:
            has_audio = True
            print("stream found!")
        else:
            print("no audio stream found.")

        if has_audio:
            print("\nextract ogg")
            subprocess.run([
                'ffmpeg', '-y', '-i', input_video, 
                '-vn', '-acodec', 'libvorbis', temp_audio
            ], check=True)
            audio_size = os.path.getsize(temp_audio)
        else:
            audio_size = 0

        print("\nreading video properties")
        video_info = subprocess.check_output([
            'ffprobe', '-v', 'error', '-select_streams', 'v:0', 
            '-show_entries', 'stream=width,height,r_frame_rate', 
            '-of', 'csv=p=0', input_video
        ]).decode().strip()
        
        parts = video_info.split(',')
        width = int(parts[0])
        height = int(parts[1])
        
        fps_val = parts[2]
        if '/' in fps_val:
            num, denom = map(float, fps_val.split('/'))
            fps = num / denom if denom != 0 else 30.0
        else:
            fps = float(fps_val)

        print("processing")

        print("\nextracting jpeg frames")

        subprocess.run([
            'ffmpeg', '-y', '-i', input_video, 
            '-q:v', '8', f'{temp_dir}/frame_%05d.jpg'
        ], check=True)

        frames = sorted([f for f in os.listdir(temp_dir) if f.endswith('.jpg')])
        frame_count = len(frames)

        print(f"\npacking into {output_gmv}")
        with open(output_gmv, 'wb') as gmv:
            # Header
            header = struct.pack('<4sIIfII', b'GMV1', width, height, fps, frame_count, audio_size)
            gmv.write(header)

            # Audio data (only if it exists)
            if has_audio and audio_size > 0:
                with open(temp_audio, 'rb') as audio_file:
                    gmv.write(audio_file.read())

            # Video frames
            for frame_name in frames:
                with open(os.path.join(temp_dir, frame_name), 'rb') as frame_file:
                    gmv.write(frame_file.read())

        print(f"\ncreated {output_gmv}!")

    except Exception as e:
        print(f"\nerror {e}")
    finally:
        if os.path.exists(temp_dir): shutil.rmtree(temp_dir)
        if os.path.exists(temp_audio): os.remove(temp_audio)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="convert videos to custom GMV format")
    
    # setup -i and -o flags as required arguments
    parser.add_argument("-i", "--input", required=True, help="input video file")
    parser.add_argument("-o", "--output", required=True, help="for the output .gmv file")
    
    args = parser.parse_args()
    
    # run the encoder with the CLI arguments
    convert_to_gmv(args.input, args.output)
