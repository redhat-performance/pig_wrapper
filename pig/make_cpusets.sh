
# set -vx

# Get list of CPUs for each NUMA node
for CPUs in $( lscpu | grep 'NUMA node' | awk '{ print $4 }' )
do
    CPUS_FOR_NODE[$NUM_NODES]="$CPUs"
    (( NUM_NODES++ ))
done

# For each PID in stdin, make a cpuset and move all of that PID's tasks into
# the cpuset.  This script assumes enough nodes exist for the amount of PIDs on
# stdin, though you could check in the while loop if (NODE==NUM_NODES)...
# It also assumes each node has sufficient resources for the PID.
(( NODE=0 ))
while read P
do
    echo "PID: $P"
    CPUSET_DIR_NAME="/sys/fs/cgroup/cpuset/cset.$P"
    # CPUSET_DIR_NAME="/cgroup/cpuset/cset.$P"
    mkdir $CPUSET_DIR_NAME
    echo "1"                       > $CPUSET_DIR_NAME/cpuset.memory_migrate
    echo "$NODE"                   > $CPUSET_DIR_NAME/cpuset.mems
    echo "${CPUS_FOR_NODE[$NODE]}" > $CPUSET_DIR_NAME/cpuset.cpus
    cd /proc/$P/task
    for TID in *
    do
	echo "Putting $TID in $CPUSET_DIR_NAME/tasks"
	echo "$TID" > $CPUSET_DIR_NAME/tasks
    done
    (( NODE++ ))
done

