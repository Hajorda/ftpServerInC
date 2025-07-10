#!/bin/zsh

cd /home/hajorda/Developer/original/client || exit

chmod +x test_client.sh

# Run the test script 5 times in parallel
for i in {1..5}; do
    ./test_client.sh &
done

# Wait for all background processes to finish
wait

echo "All tests completed."