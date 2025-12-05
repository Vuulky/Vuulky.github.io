Application 6
Trevor Carter
ID: 5036096

<iframe width="560" height="315" src="https://www.youtube.com/embed/UHv11JBSlXM?si=xADCgT2C-rUDmCqh" title="YouTube video player" frameborder="0" allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share" referrerpolicy="strict-origin-when-cross-origin" allowfullscreen></iframe>

  This real-time system showcased here is of a ride safety system that could be used within a theme park 
ride. The system monitors 3 different ways to stop the ride being the speed of the ride (modeled by the
potentiometer), an emergency stop button manned by someone that is controlling the ride from the ride 
platform, and a second emergency stop button that would be remotely manned by someone within a control
room. When the speed is too high the system will automatically trigger an emergency stop until it is 
reset by the ride operator. The system also monitors how many times the emergency stop was triggered by
speeds being too high so the company can keep metrics that could be used to determine if maintanence 
is needed.

Task Table:
                          Period        Hard/Soft         Consequence
* Speed Monitor Task:     ~1ms          Hard              If Speed is not monitored, lives could be at risk if ride is not stopped.

* E Stop Task:            ~1ms          Hard              If Button press is not registered fast enough, lives could be at risk.

* Event Task:             ~1ms          Hard              If system state is not changed fast enough then the ride might not stop in time to protect riders.

* System On Task:         ~25ms         Soft              User is not sure if the system is running 

Questions:

1. The organization of which tasks are given semaphores to write to the LEDs and change the systems state
help to have every task meet its deadline. There is an interrupt on the button that stops every other task from running
so that riders are kept safe. Each task denotes its time of being triggered and when the result happens so that the 
periods can be tracked to ensure they meet their deadline. 

2. A race condition could occur if both emergency stop buttons are pressed at similar times. This would mean that 
both are trying to modify the system state (the red led). The semaphores used protect the LED to ensure that the led is turned on
and the system stops before it moves onto another task. Since only one of these tasks lead to the correct result, avoiding them running
at the same time is ideal. 

3. Its possible to make the potentiometers deadline slip in a sense but in this theoretical ride, such rapid 
change in speed would be impossible. Since the potentiometer is not able to reset its own emergency stop
scenario, the timing in which the it meets its deadline can be strange. When it did slip the margin of time
was only a few milliseconds.

4. One feature that I wanted to add but couldnt make work is instead of having the second emergency stop button,
I wanted the website feature to show if the ride was emergency stopped or not. This feature could mimic an app/website 
that shows if a ride is currently down/delayed. This would be a feature that the riders could use, rather than one 

that would be used by operators. 
