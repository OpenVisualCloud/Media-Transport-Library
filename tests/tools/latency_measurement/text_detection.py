import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime

import cv2 as cv
import matplotlib.pyplot as plt
import numpy as np
import pytesseract


def is_display_attached():
    # Check if the DISPLAY environment variable is set
    return "DISPLAY" in os.environ


def extract_text_from_region(image, x, y, font_size, length):
    """
    Extracts text from a specific region of the image.
    :param image: The image to extract text from.
    :param x: The x-coordinate of the top-left corner of the region.
    :param y: The y-coordinate of the top-left corner of the region.
    :param font_size: The font size of the text.
    :param length: The length of the text to extract.
    :return: The extracted text.
    """
    margin = 5
    y_adjusted = max(0, y - margin)
    x_adjusted = max(0, x - margin)
    height = y + font_size + margin
    width = x + length + margin
    # Define the region of interest (ROI) for text extraction
    roi = image[y_adjusted:height, x_adjusted:width]

    # Use Tesseract to extract text from the ROI
    return pytesseract.image_to_string(roi, lang="eng")


def process_frame(frame_idx, frame):
    print("Processing Frame: ", frame_idx)

    timestamp_format = "%H:%M:%S:%f"
    timestamp_pattern = r"\b\d{2}:\d{2}:\d{2}:\d{3}\b"

    # Convert frame to grayscale for better OCR performance
    frame = cv.cvtColor(frame, cv.COLOR_BGR2GRAY)

    line_1 = extract_text_from_region(frame, 10, 10, 40, 600)
    line_2 = extract_text_from_region(frame, 10, 70, 40, 600)

    # Find the timestamps(Type: string) in the extracted text using regex
    tx_time = re.search(timestamp_pattern, line_1)
    rx_time = re.search(timestamp_pattern, line_2)

    if tx_time is None or rx_time is None:
        print("Error: Timestamp not found in the expected format.")
        return 0

    # Convert the timestamps(Type: string) to time (Type: datetime)
    tx_time = datetime.strptime(tx_time.group(), timestamp_format)
    rx_time = datetime.strptime(rx_time.group(), timestamp_format)

    if tx_time is None or rx_time is None:
        print("Error: Timestamp not found in the expected format.")
        return 0

    if tx_time > rx_time:
        print("Error: Transmit time is greater than receive time.")
        return 0

    time_difference = rx_time - tx_time
    time_difference_ms = time_difference.total_seconds() * 1000
    return time_difference_ms


def main():
    if len(sys.argv) < 2:
        print("Usage: python text-detection.py <input_video_file> <output_image_name>")
        sys.exit(1)

    input_video_file = sys.argv[1]
    cap = cv.VideoCapture(input_video_file)
    if not cap.isOpened():
        print("Fatal: Could not open video file.")
        sys.exit(1)

    frame_idx = 0
    time_differences = []

    with ThreadPoolExecutor(max_workers=40) as executor:
        futures = []
        while True:
            ret, frame = cap.read()
            if not ret:
                break

            futures.append(executor.submit(process_frame, frame_idx, frame))
            frame_idx += 1

        for future in futures:
            time_differences.append(future.result())

    # Filter out zero values from time_differences
    non_zero_time_differences = [td for td in time_differences if td != 0]

    # Calculate the average latency excluding zero values
    if non_zero_time_differences:
        average_latency = np.mean(non_zero_time_differences)

        # Filter out anomaly peaks that differ more than 25% from the average for average calculation
        filtered_time_differences = [
            td
            for td in non_zero_time_differences
            if abs(td - average_latency) <= 0.25 * average_latency
        ]

        # Calculate the average latency using the filtered data
        filtered_average_latency = np.mean(filtered_time_differences)
    else:
        print(
            "Fatal: No timestamps recognized in the video. No data for calculating latency."
        )
        sys.exit(1)

    # Plot the non-zero data
    plt.plot(non_zero_time_differences, marker="o")
    plt.title("End-to-End Latency — Media Transport Library")
    plt.xlabel("Frame Index")
    plt.ylabel("Latency, ms")
    plt.grid(True)

    # Adjust the layout to create more space for the text
    plt.subplots_adjust(bottom=0.5)

    # Prepare text for display and stdout
    average_latency_text = (
        f"Average End-to-End Latency: {filtered_average_latency:.2f} ms"
    )
    file_name = os.path.basename(input_video_file)
    file_mod_time = datetime.fromtimestamp(os.path.getmtime(input_video_file)).strftime(
        "%Y-%m-%d %H:%M:%S"
    )
    file_info_text = f"File: {file_name} | Last modified: {file_mod_time} UTC"
    width = int(cap.get(cv.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv.CAP_PROP_FPS)
    video_properties_text = f"Resolution: {width}x{height} | FPS: {fps:.2f}"

    cap.release()

    # Display text on the plot
    plt.text(
        0.5,
        -0.55,
        average_latency_text,
        horizontalalignment="center",
        verticalalignment="center",
        transform=plt.gca().transAxes,
    )
    plt.text(
        0.5,
        -0.85,
        file_info_text,
        horizontalalignment="center",
        verticalalignment="center",
        transform=plt.gca().transAxes,
    )
    plt.text(
        0.5,
        -1,
        video_properties_text,
        horizontalalignment="center",
        verticalalignment="center",
        transform=plt.gca().transAxes,
    )
    if is_display_attached():
        plt.show()

    if len(sys.argv) == 3:
        filename = sys.argv[2]
        if not filename.endswith(".jpg"):
            filename += ".jpg"
        print("Saving the latency chart to: ", filename)
        plt.savefig(filename, format="jpg", dpi=300)

    # Print text to stdout
    print(file_info_text)
    print(video_properties_text)
    print(average_latency_text)


main()
