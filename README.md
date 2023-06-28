Automation wrapper for Bill Gray's pig

Description: Bill Gray's pig tool.  Tool is designed to check migration rates of the
	     system.  For more information see pig_doc.txt and pig_examples.txt provided
             with the pig tool.
  
Location of underlying workload: Code is part of the wrapper repo.  Note it might not be the latest
version of pig.

Packages required: gcc,numactl-devel,bc

To run:
[root@hawkeye ~]# git clone https://github.com/redhat-performance/pig_wrapper
[root@hawkeye ~]# pig_wrapper/pig/run_pig.sh

Options
  --pig_opts: options to pass directly to pig
  --regression: If present, we run a limted pig test. 8 points, 120 seconds each point
  --tools_git: Pointer to the test_tools git.  Default is https://github.com/dvalinrh/test_tools.  Top directory is always test_tools
General options
  --home_parent <value>: Our parent home directory.  If not set, defaults to current working directory.
  --host_config <value>: default is the current host name.
  --iterations <value>: Number of times to run the test, defaults to 1.
  --pbench: use pbench-user-benchmark and place information into pbench, defaults to do not use.
  --pbench_user <value>: user who started everything. Defaults to the current user.
  --pbench_copy: Copy the pbench data, not move it.
  --pbench_stats: What stats to gather. Defaults to all stats.
  --run_label: the label to associate with the pbench run. No default setting.
  --run_user: user that is actually running the test on the test system. Defaults to user running wrapper.
  --sys_type: Type of system working with, aws, azure, hostname.  Defaults to hostname.
  --sysname: name of the system running, used in determing config files.  Defaults to hostname.
  --tuned_setting: used in naming the tar file, default for RHEL is the current active tuned.  For non
    RHEL systems, default is none.
  --usage: this usage message.

Note: The script does not install pbench for you.  You need to do that manually.
