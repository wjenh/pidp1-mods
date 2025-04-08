
// come here after pressing fire on title screen

rstrt,	lac (0)
		dac gmMode
		dac invFr

		jmp play


// come here upon startup and game over

play,

		// set player ship start positions

		lac (startY)
		dac yPos
		dac yPos2

		lac (startX-170)
		dac xPos
		lac (startX+170)
		dac xPos2


		// hide the bullets

		lac (true)
		dac bulHide
		dac bulHd2

		// set p2 and invader to go right

		lac (1)
		dac invRgt		
		dac shipR2

		// reset score, various other things

		lac (0)
		dac shipRgt
		dac invRow
		dac scrP1
		dac scrP1+1
		dac scrP1+2
		dac scrP2+1
		dac scrP2+2
		dac scrP2+0
		dac invXpo
		dac invFr
		dac didFire
		dac didF2
		dac hitCt
		dac spdAd

		// get invader start speed

		law spdTbl
		dap GetSpdb

		xct GetSpd
		add (bbSpd)
		dac invSpd


		// set invader start position

		lac (mxtp)
		sub (10)
		dac invYpo


		// debounce so player who pressed start doesn't fire stray bullet

		lac (-20)
		dac debo
	

		jmp main
	

200/
//main loop
main,	load \ict, -2		// initial instruction budget (delay)
		
		// check game mode

		lac gmMode
		sza
		jmp titl

// playing game

		// move the player ships 

		jsp moveShip
		jsp moveS2

		// check debounce flag, skip controls if not -1

		idx debo
		sza 
		jmp noFi

doCon,	

		jsp controls
		lac (-1)
		dac debo
		
		jsp checkFire
		jsp chckF2


noFi,
		// draw the player ships

		jsp drawShip				
		jsp drawS2

		jsp drawStar	// draw flickering stars
		jsp drawInv		// draw invader (and move)


		// move any alive bullets

		jsp moveBullt
		jsp moveB2

		// draw any alive bullets

		jsp drawBullet
		jsp drawB2

		// check bullet to invader collisions

		jsp checkCol
		jsp chckC2

		// go to end of main loop

		jmp endMn



titl,
		
		// draw invader on title screen

		jsp drawInv

		// check control debounce

		idx debo
		sza 
		jmp debou

okay,	

		jsp controls
		

		// no debounce needed after first timer

		lac (-1)
		dac debo


		// set ai to off as default

		lac (0)
		dac aiOff

		// check P1 start pressed, jmp to rstrt if true

		lac didFire
		sza
		jmp rstrt

		// ai will be on if 2P start

		lac (1)
		dac aiOff

		// check P2 start pressed, jmp to rstrt if true
		lac didF2
		sza
		jmp rstrt

debou,

		// no button pressed, do we draw title this 'frame'?
		idx flsh
		sza
		jmp endMn

		lac (-txtfl)
		dac flsh

		jsp drwTi


endMn,

		idx countr

		count \ict, .			// use up rest of time of main loop
		jmp main				// next frame 

