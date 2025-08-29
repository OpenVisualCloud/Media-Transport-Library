import csv
import os

class TestCSVReport:
    def __init__(self, csv_path):
        """
        Initialize the CSV report.
        If the file does not exist, create it and write the header row.
        """
        self.csv_path = csv_path
        # Write header if file does not exist
        if not os.path.isfile(self.csv_path):
            with open(self.csv_path, mode='w', newline='') as csvfile:
                writer = csv.writer(csvfile)
                writer.writerow([
                    "test_name",           # Name of the test
                    "compliance_result",   # Compliance check result (e.g., PASSED/FAILED)
                    "logs_path"            # Final result (e.g., PASSED/FAILED)
                ])

    def add_result(self, test_name, compliance_result, logs_path):
        """
        Append a new row with the test results to the CSV file.
        """
        with open(self.csv_path, mode='a', newline='') as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow([
                test_name,
                compliance_result,
                logs_path
            ])
