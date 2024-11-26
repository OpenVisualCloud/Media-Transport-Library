import platform
import subprocess

def check_yuv_mount_path():
    if platform.system().lower() == "windows":
        check_yuv_cmd = r'net use | findstr "10.67.116.200" | findstr "OK"'
        mount_yuv_cmd = r'net use z: \\10.67.116.200\datadisk\streams intel123 /user:media' 
    else:
        check_yuv_cmd = 'df -h | grep "/home/gta/IMTL/media"'
        mount_yuv_cmd = 'sudo mount -o vers=3 10.67.116.200:/datadisk/streams/kahawai/yuvs /home/gta/IMTL/media'
    check_prcoess = subprocess.Popen(check_yuv_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=True)
    check_result = check_prcoess.stdout.read()
    print(check_result)
    if 'streams' in str(check_result):
        print("The yuv path has been mounted")
        print(check_result)
        return 0
    else:
        print("Mounting the yuvs path......")
        mount_prcocess = subprocess.Popen(mount_yuv_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=True)
        mount_prcocess.communicate()
        check_prcoess = subprocess.Popen(check_yuv_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=True)
        check_result = check_prcoess.stdout.read()
        if 'streams' in str(check_result):
            print("Mount successfully")
            print(check_result)
            return 0
        else:
            print("Mount failed")
            return 1



if __name__ == '__main__':
   aa =  check_yuv_mount_path()
   print(aa)






