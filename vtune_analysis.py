#!/usr/bin/env python3

import subprocess
import os
import sys
import shlex
import shutil
import argparse
import time
import json
import re
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Tuple

def setup_vtune_environment():
    """Source VTune environment variables."""
    vtune_vars_script = "/opt/intel/oneapi/vtune/latest/vtune-vars.sh"
    if os.path.exists(vtune_vars_script):
        cmd = f"source {vtune_vars_script} && env"
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True, executable='/bin/bash')
        if result.returncode == 0:
            for line in result.stdout.split('\n'):
                if '=' in line and not line.startswith('_'):
                    key, value = line.split('=', 1)
                    os.environ[key] = value
            print("✓ VTune environment variables loaded successfully")
        else:
            print(f"Warning: Could not source VTune environment: {result.stderr}")
    else:
        print(f"Warning: VTune environment script not found at {vtune_vars_script}")

setup_vtune_environment()

def read_config_file(config_file: str) -> Tuple[str, str]:
    """Read configuration from file."""
    try:
        with open(config_file, 'r') as f:
            lines = [line.strip() for line in f.readlines() if line.strip() and not line.startswith('#')]
        
        if len(lines) < 2:
            raise ValueError("Config file must contain at least 2 lines: command and working directory")
        
        app_command, app_working_dir = lines[0], lines[1]
        print(f"Read from config file:\n  Command: {app_command}\n  Working Directory: {app_working_dir}")
        return validate_paths(app_command, app_working_dir)
        
    except FileNotFoundError:
        print(f"Error: Config file '{config_file}' not found")
        sys.exit(1)
    except Exception as e:
        print(f"Error reading config file: {e}")
        sys.exit(1)

def validate_paths(app_command: str, app_working_dir: str) -> Tuple[str, str]:
    """Validate that the application and working directory exist."""
    cmd_parts = shlex.split(app_command)
    if not cmd_parts:
        raise ValueError("Empty command")
    
    executable = cmd_parts[0]
    if not os.path.isabs(executable):
        executable = os.path.abspath(executable)
    if not os.path.isabs(app_working_dir):
        app_working_dir = os.path.abspath(app_working_dir)
    
    if not os.path.isfile(executable):
        raise ValueError(f"Executable not found: {executable}")
    if not os.access(executable, os.X_OK):
        raise ValueError(f"File is not executable: {executable}")
    if not os.path.isdir(app_working_dir):
        raise ValueError(f"Working directory not found: {app_working_dir}")
    
    print(f"✓ Validated executable: {executable}\n✓ Validated working directory: {app_working_dir}")
    updated_cmd_parts = [executable] + cmd_parts[1:]
    updated_app_command = ' '.join(shlex.quote(part) for part in updated_cmd_parts)
    
    return updated_app_command, app_working_dir

class VTuneAnalyzer:
    def __init__(self, app_command: str, app_working_dir: str, duration: int = 90):
        self.app_command = app_command
        self.app_working_dir = app_working_dir
        self.duration = duration
        self.results_dir = Path("vtune_results")
        self.results_dir.mkdir(exist_ok=True)
        
        self.analyses = {
            'io': {
            'name': 'I/O Analysis', 'collect': 'io', 'description': 'Analyzes I/O operations and disk access patterns', 'timeout_multiplier': 2.5,
            'fallback_modes': [('io', 'Full I/O analysis'), ('disk-io', 'Disk I/O analysis only'), ('system-overview', 'System overview with I/O metrics')]
            },
            'memory-access': {'name': 'Memory Access Analysis', 'collect': 'memory-access', 'description': 'Analyzes memory access patterns and cache behavior', 'timeout_multiplier': 1.5},
            'uarch-exploration': {'name': 'Microarchitecture Exploration', 'collect': 'uarch-exploration', 'description': 'Analyzes microarchitecture utilization and bottlenecks', 'timeout_multiplier': 1.3},
            'memory-consumption': {'name': 'Memory Consumption', 'collect': 'memory-consumption', 'description': 'Analyzes memory allocation and usage patterns', 'timeout_multiplier': 1.5}
        }
        self.results = {}
    
    def get_system_info(self) -> Dict[str, str]:
        """Collect system information."""
        info = {}
        try:
            # CPU information
            with open('/proc/cpuinfo', 'r') as f:
                cpuinfo = f.read()
            cpu_match = re.search(r'model name\s*:\s*(.+)', cpuinfo)
            if cpu_match:
                info['CPU Model'] = cpu_match.group(1).strip()
            info['Logical Cores'] = str(len(re.findall(r'^processor\s*:', cpuinfo, re.MULTILINE)))
            
            # Memory information
            with open('/proc/meminfo', 'r') as f:
                meminfo = f.read()
            mem_match = re.search(r'MemTotal:\s*(\d+)\s*kB', meminfo)
            if mem_match:
                mem_gb = int(mem_match.group(1)) / 1024 / 1024
                info['Total Memory'] = f"{mem_gb:.1f} GB"
            
            # OS and kernel information
            try:
                with open('/etc/os-release', 'r') as f:
                    os_info = f.read()
                pretty_name = re.search(r'PRETTY_NAME="([^"]+)"', os_info)
                if pretty_name:
                    info['OS'] = pretty_name.group(1)
            except:
                info['OS'] = 'Unknown'
            
            try:
                info['Kernel'] = subprocess.check_output(['uname', '-r'], text=True).strip()
            except:
                info['Kernel'] = 'Unknown'
                
            # VTune version
            try:
                result = subprocess.run(['vtune', '--version'], capture_output=True, text=True)
                version_match = re.search(r'Intel.*VTune.*(\d+\.\d+\.\d+)', result.stdout)
                info['VTune Version'] = version_match.group(1) if version_match else 'Unknown'
            except:
                info['VTune Version'] = 'Unknown'
                
        except Exception as e:
            print(f"Warning: Could not gather complete system info: {e}")
        return info
    
    def run_vtune_analysis(self, analysis_key: str) -> Dict[str, str]:
        """Run a specific VTune analysis."""
        analysis = self.analyses[analysis_key]
        result_dir = self.results_dir / f"r_{analysis_key}"
        
        print(f"\n{'='*60}")
        print(f"Running {analysis['name']}...")
        print(f"Description: {analysis['description']}")
        print(f"Duration: {self.duration} seconds")
        print(f"{'='*60}")
        
        if result_dir.exists():
            print(f"Removing existing result directory: {result_dir}")
            shutil.rmtree(result_dir)
        
        # Get fallback modes if available, otherwise use single mode
        modes = analysis.get('fallback_modes', [(analysis['collect'], analysis['name'])])
        
        for i, (collect_mode, description) in enumerate(modes):
            if len(modes) > 1:
                print(f"\nAttempt {i+1}: {description}")
            
            current_analysis = analysis.copy()
            current_analysis['collect'] = collect_mode
            result = self._run_single_vtune_analysis(current_analysis, result_dir, analysis_key)
            
            if result.get('Status') == 'Success':
                return result
        
        # If we get here, all attempts failed
        if len(modes) > 1:
            print(f"✗ All {analysis['name']} attempts failed")
            return {'Status': 'Failed', 'Error': f'All {analysis["name"]} modes failed or timed out'}
        else:
            return result
    
    
    def _run_single_vtune_analysis(self, analysis: dict, result_dir: Path, analysis_key: str) -> Dict[str, str]:
        """Run a single VTune analysis attempt."""
        vtune_cmd = ['vtune', '-collect', analysis['collect'], '-result-dir', str(result_dir.absolute()), '-duration', str(self.duration), '-app-working-dir', os.path.abspath(self.app_working_dir), '--'] + shlex.split(self.app_command)
        
        timeout_multiplier = analysis.get('timeout_multiplier', 1.5)
        analysis_timeout = int(self.duration * timeout_multiplier + 60)
        
        print(f"Command: {' '.join(vtune_cmd)}")
        print(f"Working Directory: {os.path.abspath(self.app_working_dir)}")
        print(f"Result Directory: {result_dir.absolute()}")
        
        result_dir.parent.mkdir(parents=True, exist_ok=True)
        
        start_time = time.time()
        try:
            result = subprocess.run(vtune_cmd, timeout=analysis_timeout, capture_output=True, text=True, cwd=os.path.abspath(self.app_working_dir))
            elapsed_time = time.time() - start_time
            
            if result.returncode == 0:
                print(f"✓ Analysis completed successfully in {elapsed_time:.1f}s")
                return self._parse_vtune_results(result_dir, analysis_key)
            else:
                print(f"✗ Analysis failed with return code {result.returncode}")
                print(f"STDERR: {result.stderr}")
                return {'Status': 'Failed', 'Error': f"VTune failed with code {result.returncode}: {result.stderr}"}
        except subprocess.TimeoutExpired:
            print(f"✗ Analysis timed out after {analysis_timeout}s")
            return {'Status': 'Failed', 'Error': f'Analysis timed out after {analysis_timeout} seconds'}
        except Exception as e:
            print(f"✗ Analysis failed with exception: {e}")
            return {'Status': 'Failed', 'Error': str(e)}
    
    def _parse_vtune_results(self, result_dir: Path, analysis_key: str) -> Dict[str, str]:
        """Parse VTune results from the result directory."""
        results = {'Status': 'Success'}
        
        try:
            summary_cmd = ['vtune', '-report', 'summary', '-result-dir', str(result_dir)]
            summary_result = subprocess.run(summary_cmd, capture_output=True, text=True, timeout=60)
            
            if summary_result.returncode == 0:
                summary = summary_result.stdout
                if analysis_key == 'io':
                    results.update(self._parse_io_summary(summary))
                elif analysis_key == 'memory-access':
                    results.update(self._parse_patterns(summary, self._get_memory_access_patterns()))
                elif analysis_key == 'uarch-exploration':
                    results.update(self._parse_patterns(summary, self._get_uarch_patterns()))
                elif analysis_key == 'memory-consumption':
                    results.update(self._parse_patterns(summary, self._get_memory_consumption_patterns()))
            else:
                results['Status'] = 'Failed'
                results['Error'] = f"Failed to generate summary: {summary_result.stderr}"
                
        except Exception as e:
            results['Status'] = 'Failed'
            results['Error'] = f"Failed to parse results: {str(e)}"
        
        return results
    
    def _parse_patterns(self, summary: str, patterns: dict) -> Dict[str, str]:
        """Generic pattern parsing for all analysis types."""
        results = {}
        for key, pattern in patterns.items():
            match = re.search(pattern, summary, re.IGNORECASE)
            if match:
                value = match.group(1)
                # Add units based on the metric type and pattern
                results[key] = self._format_value_with_unit(key, value)
        return results
    
    def _parse_io_summary(self, summary: str) -> Dict[str, str]:
        """Parse I/O analysis summary with specialized patterns."""
        results = {}
        
        # PCIe Traffic metrics
        io_patterns = {
            'Inbound PCIe Read': r'Inbound PCIe Read, MB/sec:\s*([0-9.]+)',
            'Inbound PCIe Write': r'Inbound PCIe Write, MB/sec:\s*([0-9.]+)',
            'Outbound PCIe Read': r'Outbound PCIe Read, MB/sec:\s*([0-9.]+)',
            'Outbound PCIe Write': r'Outbound PCIe Write, MB/sec:\s*([0-9.]+)',
            'Inbound PCIe Read L3 Hit': r'Inbound PCIe Read, MB/sec:.*?\n.*?L3 Hit, %:\s*([0-9.]+)',
            'Inbound PCIe Read L3 Miss': r'Inbound PCIe Read, MB/sec:.*?\n.*?L3 Miss, %:\s*([0-9.]+)',
            'Inbound PCIe Read Average Latency': r'Inbound PCIe Read, MB/sec:.*?Average Latency, ns:\s*([0-9.]+)',
            'Inbound PCIe Write L3 Hit': r'Inbound PCIe Write, MB/sec:.*?\n.*?L3 Hit, %:\s*([0-9.]+)',
            'Inbound PCIe Write L3 Miss': r'Inbound PCIe Write, MB/sec:.*?\n.*?L3 Miss, %:\s*([0-9.]+)',
            'Inbound PCIe Write Average Latency': r'Inbound PCIe Write, MB/sec:.*?Average Latency, ns:\s*([0-9.]+)',
            'Effective Physical Core Utilization': r'Effective Physical Core Utilization:\s*([0-9.]+)%\s*\(([0-9.]+)\s*out\s*of\s*([0-9]+)\)',
            'Effective Logical Core Utilization': r'Effective Logical Core Utilization:\s*([0-9.]+)%\s*\(([0-9.]+)\s*out\s*of\s*([0-9]+)\)',
        }
        
        for key, pattern in io_patterns.items():
            match = re.search(pattern, summary, re.DOTALL)
            if match:
                # Add appropriate units based on the metric type
                if 'PCIe Read' in key or 'PCIe Write' in key:
                    if 'L3 Hit' in key or 'L3 Miss' in key:
                        results[key] = f"{match.group(1)}%"
                    elif 'Average Latency' in key:
                        results[key] = f"{match.group(1)} ns"
                    else:
                        results[key] = f"{match.group(1)} MB/s"
                else:
                    results[key] = match.group(1)
        
        # Bandwidth utilization table parsing - exact format matching
        bandwidth_section = re.search(r'Bandwidth Utilization\n.*?\n.*?\n(.*?)(?=\n\nTop|collection|Collection|\Z)', summary, re.DOTALL | re.IGNORECASE)
        if bandwidth_section:
            table_content = bandwidth_section.group(1).strip()
            for line in table_content.split('\n'):
                line = line.strip()
                if not line or line.startswith('-'):
                    continue
                
                # Parse table format using regex to handle whitespace properly
                if 'DRAM, GB/sec' in line:
                    # Extract numbers from: "DRAM, GB/sec    350    22.600    4.863    0.0%"
                    match = re.search(r'DRAM, GB/sec\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)%', line)
                    if match:
                        results['DRAM Platform Maximum'] = f"{match.group(1)} GB/s"
                        results['DRAM Observed Maximum'] = f"{match.group(2)} GB/s"
                        results['DRAM Average'] = f"{match.group(3)} GB/s"
                        results['DRAM High BW Utilization'] = f"{match.group(4)}%"
                
                elif 'DRAM Single-Package, GB/sec' in line:
                    # Extract numbers from: "DRAM Single-Package, GB/sec    175    19.900    5.734    0.0%"
                    match = re.search(r'DRAM Single-Package, GB/sec\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)%', line)
                    if match:
                        results['DRAM Single-Package Platform Maximum'] = f"{match.group(1)} GB/s"
                        results['DRAM Single-Package Observed Maximum'] = f"{match.group(2)} GB/s"
                        results['DRAM Single-Package Average'] = f"{match.group(3)} GB/s"
                        results['DRAM Single-Package High BW Utilization'] = f"{match.group(4)}%"
                
                elif 'UPI Utilization Single-link' in line:
                    # Extract numbers from: "UPI Utilization Single-link, (%)    100    19.600    3.711    0.0%"
                    match = re.search(r'UPI Utilization Single-link.*?\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)%', line)
                    if match:
                        results['UPI Platform Maximum'] = f"{match.group(1)}%"
                        results['UPI Observed Maximum'] = f"{match.group(2)}%"
                        results['UPI Average'] = f"{match.group(3)}%"
                        results['UPI High BW Utilization'] = f"{match.group(4)}%"
                
                elif 'PCIe Bandwidth, MB/sec' in line:
                    # Extract numbers from: "PCIe Bandwidth, MB/sec    40    689.700    430.933    91.2%"
                    match = re.search(r'PCIe Bandwidth, MB/sec\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)%', line)
                    if match:
                        results['PCIe Platform Maximum'] = f"{match.group(1)} MB/s"
                        results['PCIe Observed Maximum'] = f"{match.group(2)} MB/s"
                        results['PCIe Average'] = f"{match.group(3)} MB/s"
                        results['PCIe High BW Utilization'] = f"{match.group(4)}%"
        
        # Parse core utilization with multiple groups
        phys_core_match = re.search(r'Effective Physical Core Utilization:\s*([0-9.]+)%\s*\(([0-9.]+)\s*out\s*of\s*([0-9]+)\)', summary)
        if phys_core_match:
            results['Effective Physical Core Utilization'] = f"{phys_core_match.group(1)}%"
            results['Effective Physical Cores Used'] = f"{phys_core_match.group(2)} cores"
            results['Total Physical Cores'] = f"{phys_core_match.group(3)} cores"
        
        log_core_match = re.search(r'Effective Logical Core Utilization:\s*([0-9.]+)%\s*\(([0-9.]+)\s*out\s*of\s*([0-9]+)\)', summary)
        if log_core_match:
            results['Effective Logical Core Utilization'] = f"{log_core_match.group(1)}%"
            results['Effective Logical Cores Used'] = f"{log_core_match.group(2)} cores"
            results['Total Logical Cores'] = f"{log_core_match.group(3)} cores"
        
        return results
    
    def _get_memory_access_patterns(self) -> dict:
        """Memory Access analysis patterns."""
        return {
            'Memory Bound': r'Memory Bound:\s*([0-9.]+)%',
            'L1 Bound': r'L1 Bound:\s*([0-9.]+)%',
            'L2 Bound': r'L2 Bound:\s*([0-9.]+)%',
            'L3 Bound': r'L3 Bound:\s*([0-9.]+)%',
            'DRAM Bound': r'DRAM Bound:\s*([0-9.]+)%',
            'Store Bound': r'Store Bound:\s*([0-9.]+)%',
            'NUMA Remote Accesses': r'NUMA.*Remote.*Accesses.*:\s*([0-9.]+)%',
            'Remote Accesses': r'Remote.*Accesses.*:\s*([0-9.]+)%',
            'LLC Miss Count': r'LLC.*Miss.*Count:\s*([0-9,]+)',
            'Loads': r'Loads:\s*([0-9,]+)',
            'Stores': r'Stores:\s*([0-9,]+)'
        }
    
    def _get_uarch_patterns(self) -> dict:
        """Microarchitecture Exploration patterns."""
        return {
            'CPI Rate': r'CPI Rate:\s*([0-9.]+)',
            'Retiring': r'Retiring:\s*([0-9.]+)%',
            'Bad Speculation': r'Bad Speculation:\s*([0-9.]+)%',
            'Front-End Bound': r'Front-End Bound:\s*([0-9.]+)%',
            'Back-End Bound': r'Back-End Bound:\s*([0-9.]+)%',
            'FP Arithmetic': r'FP Arithmetic:\s*([0-9.]+)%',
            'Memory Operations': r'Memory Operations:\s*([0-9.]+)%',
            'Branch Instructions': r'Branch Instructions:\s*([0-9.]+)%',
            'NOP Instructions': r'NOP Instructions:\s*([0-9.]+)%',
            'Other': r'Other:\s*([0-9.]+)%',
            'Core Bound': r'Core Bound:\s*([0-9.]+)%',
            'Divider': r'Divider:\s*([0-9.]+)%',
            'Cycles of 0 Ports Utilized': r'Cycles of 0 Ports Utilized:\s*([0-9.]+)%',
            'Cycles of 1 Port Utilized': r'Cycles of 1 Port Utilized:\s*([0-9.]+)%',
            'Cycles of 2 Ports Utilized': r'Cycles of 2 Ports Utilized:\s*([0-9.]+)%',
            'Cycles of 3+ Ports Utilized': r'Cycles of 3\+ Ports Utilized:\s*([0-9.]+)%',
            'Lock Latency': r'Lock Latency:\s*([0-9.]+)%',
            'SQ Full': r'SQ Full:\s*([0-9.]+)%'
        }
    
    
    def _get_memory_consumption_patterns(self) -> dict:
        """Memory Consumption analysis patterns."""
        return {
            'Allocation Size': r'Allocation Size:\s*([0-9.]+\s*[KMGT]?B)',
            'Deallocation Size': r'Deallocation Size:\s*([0-9.]+\s*[KMGT]?B)'
        }
    
    def _format_value_with_unit(self, key: str, value: str) -> str:
        """Add appropriate units to values that might be missing them."""
        if any(unit in value.upper() for unit in ['B', 'KB', 'MB', 'GB', 'TB', '%', 'MS', 'S', '/S']):
            return value
        
        # Common unit mappings
        unit_map = {
            'Time': 's', 'Latency': 'ns', 'Hit Rate': '%', 'Bound': '%', 'Utilization': '%',
            'CPI': '', 'IPC': '', 'Operations': 'ops', 'Instructions': 'ops', 'Count': '',
            'Bandwidth': 'B/s', 'FLOPS': 'FLOPS', 'GFLOPS': 'GFLOPS', 'Frequency': 'GHz'
        }
        
        for keyword, unit in unit_map.items():
            if keyword in key:
                return f"{value} {unit}" if unit else value
        
        return value
    
    def _format_category_results(self, result: Dict[str, str], categories: dict) -> List[str]:
        """Generic category-based result formatting."""
        formatted_lines = []
        
        for category_name, category_info in categories.items():
            category_metrics = {key: result[key] for key in category_info['metrics'] if key in result}
            
            if category_metrics:
                formatted_lines.append(f"  {category_name}:")
                formatted_lines.append(f"    {category_info['explanation']}")
                for key, value in category_metrics.items():
                    formatted_value = self._format_value_with_unit(key, value)
                    formatted_lines.append(f"    {key:40} {formatted_value}")
                formatted_lines.append("")
        
        # Add remaining metrics
        all_categorized_keys = set()
        for category_info in categories.values():
            all_categorized_keys.update(category_info['metrics'])
        
        remaining_metrics = {key: value for key, value in result.items() if key != 'Status' and key not in all_categorized_keys}
        if remaining_metrics:
            formatted_lines.append("  Other Metrics:")
            for key, value in remaining_metrics.items():
                formatted_value = self._format_value_with_unit(key, value)
                formatted_lines.append(f"    {key:40} {formatted_value}")
            formatted_lines.append("")
        
        return formatted_lines
    
    def _format_io_results(self, result: Dict[str, str]) -> List[str]:
        """Format I/O analysis results."""
        categories = {
            'PCIe Traffic Summary': {
                'metrics': ['Inbound PCIe Read', 'Inbound PCIe Write', 'Outbound PCIe Read', 'Outbound PCIe Write', 'PCIe Platform Maximum', 'PCIe Observed Maximum', 'PCIe Average', 'PCIe High BW Utilization'],
                'explanation': 'PCIe bus traffic analysis showing inbound/outbound data movement and bandwidth utilization'
            },
            'PCIe Cache Performance': {
                'metrics': ['Inbound PCIe Read L3 Hit', 'Inbound PCIe Read L3 Miss', 'Inbound PCIe Write L3 Hit', 'Inbound PCIe Write L3 Miss', 'Inbound PCIe Read Average Latency', 'Inbound PCIe Write Average Latency'],
                'explanation': 'PCIe cache hit/miss rates and memory access latency for PCIe operations'
            },
            'DRAM Bandwidth Utilization': {
                'metrics': ['DRAM Platform Maximum', 'DRAM Observed Maximum', 'DRAM Average', 'DRAM High BW Utilization', 'DRAM Single-Package Platform Maximum', 'DRAM Single-Package Observed Maximum', 'DRAM Single-Package Average', 'DRAM Single-Package High BW Utilization'],
                'explanation': 'DRAM memory bandwidth usage across NUMA nodes and memory packages'
            },
            'UPI Utilization': {
                'metrics': ['UPI Platform Maximum', 'UPI Observed Maximum', 'UPI Average', 'UPI High BW Utilization'],
                'explanation': 'Ultra Path Interconnect (UPI) bandwidth utilization between CPU packages'
            },
            'Core Utilization': {
                'metrics': ['Effective Physical Core Utilization', 'Effective Physical Cores Used', 'Total Physical Cores', 'Effective Logical Core Utilization', 'Effective Logical Cores Used', 'Total Logical Cores'],
                'explanation': 'CPU core utilization during I/O operations showing physical and logical core usage'
            }
        }
        return self._format_category_results(result, categories)
    
    def _format_memory_access_results(self, result: Dict[str, str]) -> List[str]:
        """Format Memory Access analysis results."""
        categories = {
            'Memory Hierarchy Bottlenecks': {
                'metrics': ['Memory Bound', 'L1 Bound', 'L2 Bound', 'L3 Bound', 'DRAM Bound', 'Store Bound'],
                'explanation': 'Shows where memory accesses are bottlenecked in the hierarchy'
            },
            'Cache Performance': {
                'metrics': ['LLC Miss Count'],
                'explanation': 'Cache hit rates and miss counts across memory hierarchy levels'
            },
            'NUMA Performance': {
                'metrics': ['NUMA Remote Accesses', 'Remote Accesses'],
                'explanation': 'Percentage of memory accesses that go to remote NUMA nodes'
            },
            'Memory Operations': {
                'metrics': ['Loads', 'Stores'],
                'explanation': 'Count of load and store operations performed by the application'
            }
        }
        return self._format_category_results(result, categories)
    
    def _format_uarch_results(self, result: Dict[str, str]) -> List[str]:
        """Format Microarchitecture Exploration results."""
        categories = {
            'CPI Performance Metrics': {
                'metrics': ['CPI Rate'],
                'explanation': 'Cycles Per Instruction (CPI) and related metrics - lower CPI/higher IPC indicates better performance'
            },
            'Top-Down Analysis': {
                'metrics': ['Retiring', 'Bad Speculation', 'Front-End Bound', 'Back-End Bound'],
                'explanation': 'Top-down microarchitecture analysis - should sum to ~100%'
            },
            'Back-End Core Bound': {
                'metrics': ['Core Bound', 'Divider', 'Lock Latency', 'Cycles of 0 Ports Utilized', 'Cycles of 1 Port Utilized', 'Cycles of 2 Ports Utilized', 'Cycles of 3+ Ports Utilized', 'SQ Full'],
                'explanation': 'CPU execution units and resource contention bottlenecks'
            }
        }
        return self._format_category_results(result, categories)
    
    def _format_memory_consumption_results(self, result: Dict[str, str]) -> List[str]:
        """Format Memory Consumption results."""
        categories = {
            'Memory Usage Statistics': {
                'metrics': ['Allocation Size', 'Deallocation Size'],
                'explanation': 'Memory usage statistics showing allocation, deallocation, and peak consumption'
            }
        }
        return self._format_category_results(result, categories)
    
    def generate_report(self) -> str:
        """Generate a comprehensive report of all analyses."""
        report = []
        
        # Header
        report.extend([
            "=" * 80,
            "VTune Analysis Report",
            "=" * 80,
            f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            f"Application: {self.app_command}",
            f"Analysis Duration: {self.duration} seconds each",
            ""
        ])
        
        # System Information
        report.append("SYSTEM INFORMATION")
        report.append("-" * 40)
        system_info = self.get_system_info()
        for key, value in system_info.items():
            report.append(f"{key:20} {value}")
        report.append("")
        
        # Analysis Results
        report.append("ANALYSIS RESULTS")
        report.append("-" * 40)
        
        format_map = {
            'io': self._format_io_results,
            'memory-access': self._format_memory_access_results,
            'uarch-exploration': self._format_uarch_results,
            'memory-consumption': self._format_memory_consumption_results
        }
        
        for analysis_key, analysis_info in self.analyses.items():
            result = self.results.get(analysis_key, {})
            report.append(f"{analysis_info['name']}:")
            
            if result.get('Status') == 'Success':
                report.append("  Status: ✓ Success")
                if analysis_key in format_map:
                    report.extend(format_map[analysis_key](result))
                else:
                    for key, value in result.items():
                        if key != 'Status':
                            formatted_value = self._format_value_with_unit(key, value)
                            report.append(f"  {key:40} {formatted_value}")
            else:
                report.append(f"  Status: ✗ Failed")
                if 'Error' in result:
                    report.append(f"  Error: {result['Error']}")
            report.append("")
        
        return "\n".join(report)
    
    def run_all_analyses(self):
        """Run all VTune analyses."""
        print("Starting VTune Analysis Suite")
        print(f"Application: {self.app_command}")
        print(f"Working Directory: {self.app_working_dir}")
        print(f"Duration per analysis: {self.duration} seconds")
        
        # Run each analysis
        for i, analysis_key in enumerate(self.analyses.keys()):
            print(f"\nProgress: [{i+1}/{len(self.analyses)}]")
            self.results[analysis_key] = self.run_vtune_analysis(analysis_key)
        
        # Generate and display report
        report = self.generate_report()
        print("\n" + report)
        
        # Save report and results
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        report_file = self.results_dir / f"vtune_analysis_report_{timestamp}.txt"
        json_file = self.results_dir / f"vtune_analysis_results_{timestamp}.json"
        
        with open(report_file, 'w') as f:
            f.write(report)
        
        with open(json_file, 'w') as f:
            json.dump({
                'timestamp': timestamp,
                'app_command': self.app_command,
                'app_working_dir': self.app_working_dir,
                'duration': self.duration,
                'system_info': self.get_system_info(),
                'results': self.results
            }, f, indent=2)
        
        print(f"\nReport saved to: {report_file}")
        print(f"Results saved to: {json_file}")

def main():
    parser = argparse.ArgumentParser(
        description='Run comprehensive VTune analysis on an application',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Example usage:
  python vtune_analysis.py config.txt
  python vtune_analysis.py --duration 120 config.txt

Config file format (two lines):
  ./tests/tools/RxTxApp/build/RxTxApp --config /root/awilczyn/Media-Transport-Library/config/tx_1v.json
  ./tests/tools/RxTxApp/build
        """
    )
    
    parser.add_argument('config_file', help='Configuration file containing command and working directory')
    parser.add_argument('--duration', '-d', type=int, default=90, help='Duration in seconds for each analysis (default: 90)')
    
    args = parser.parse_args()
    
    # Read configuration from file
    app_command, app_working_dir = read_config_file(args.config_file)
    
    # Check if VTune is available
    try:
        subprocess.run(['vtune', '--version'], capture_output=True, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("Error: VTune is not available or not in PATH")
        sys.exit(1)
    
    # Run analysis
    analyzer = VTuneAnalyzer(app_command, app_working_dir, args.duration)
    analyzer.run_all_analyses()

if __name__ == '__main__':
    main()
