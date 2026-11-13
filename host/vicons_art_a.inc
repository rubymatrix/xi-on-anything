/* Status icon art (host/vicons.c): ART(status id, SVG contents on a 64-unit square). */

/* 0: ko - a pale iridescent opal egg */
ART(0,
    "<defs>"
    "<radialGradient id='o' cx='40%' cy='30%' r='72%' fx='40%' fy='30%'>"
    "<stop offset='0' stop-color='#EEF4FA'/>"
    "<stop offset='0.5' stop-color='#94A6C8'/>"
    "<stop offset='1' stop-color='#3E3E6E'/>"
    "</radialGradient>"
    "</defs>"
    "<ellipse transform='translate(32 32) rotate(-14)' rx='24' ry='29.5' fill='url(#o)' stroke='#0E0C22' stroke-width='2.5'/>"
    "<ellipse transform='translate(22 40) rotate(-30)' rx='6' ry='9' fill='#F08CA8' fill-opacity='0.55'/>"
    "<ellipse transform='translate(42 26) rotate(20)' rx='5' ry='8' fill='#5CE0E8' fill-opacity='0.5'/>"
    "<ellipse transform='translate(38 46) rotate(-20)' rx='6' ry='4' fill='#B48CF0' fill-opacity='0.55'/>"
    "<ellipse transform='translate(25 22) rotate(-30)' rx='5' ry='3' fill='#F6E08A' fill-opacity='0.5'/>"
    "<ellipse transform='translate(26 15) rotate(-20)' rx='8' ry='4.5' fill='#FFFFFF' fill-opacity='0.35'/>")

/* 1: weakness - a pale teal skull */
ART(1,
    "<radialGradient id='s' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#E6FAF4'/>"
    "<stop offset='0.5' stop-color='#8CC8BC'/>"
    "<stop offset='1' stop-color='#3E7A70'/>"
    "</radialGradient>"
    "<path fill='url(#s)' stroke='#0A1E1A' stroke-width='2.5' stroke-linejoin='round' d='M32 4 C17 4 7.5 14 7.5 27 C7.5 35 10.5 40 15 43 L16 52 C16 56.5 19 59.5 23.5 59.5 L40.5 59.5 C45 59.5 48 56.5 48 52 L49 43 C53.5 40 56.5 35 56.5 27 C56.5 14 47 4 32 4 Z'/>"
    "<ellipse cx='25' cy='14' rx='10' ry='5' fill='#FFFFFF' fill-opacity='0.25'/>"
    "<ellipse cx='21.5' cy='30' rx='7.5' ry='7' fill='#14302C'/>"
    "<ellipse cx='42.5' cy='30' rx='7.5' ry='7' fill='#14302C'/>"
    "<path fill='#14302C' stroke='#14302C' stroke-width='1.5' stroke-linejoin='round' d='M32 36.5 L28.5 43.5 L35.5 43.5 Z'/>"
    "<rect x='21' y='47' width='22' height='8.5' rx='2' fill='#14302C'/>"
    "<path fill='none' stroke='#8CC8BC' stroke-width='2' d='M26.5 47.5 V55 M32 47.5 V55 M37.5 47.5 V55'/>")

/* 2: sleep, sleep II - a white thought bubble with a violet "Zz" */
ART(2,
    "<radialGradient id='c' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#FFFFFF'/>"
    "<stop offset='0.5' stop-color='#E8E8F2'/>"
    "<stop offset='1' stop-color='#A4A4BC'/>"
    "</radialGradient>"
    "<path fill='url(#c)' stroke='#14142A' stroke-width='2.4' stroke-linejoin='round' d='M12 8 H50 Q59 8 59 17 V40 Q59 49 50 49 H48 L55 59 L38 49 H12 Q4 49 4 40 V17 Q4 8 12 8 Z'/>"
    "<ellipse cx='24' cy='14' rx='13' ry='3.5' fill='#FFFFFF' fill-opacity='0.6'/>"
    "<path fill='none' stroke='#22104A' stroke-width='9.6' stroke-linecap='round' stroke-linejoin='round' d='M13 16 H31 L13 41 H31'/>"
    "<path fill='none' stroke='#7A4ACC' stroke-width='6.2' stroke-linecap='round' stroke-linejoin='round' d='M13 16 H31 L13 41 H31'/>"
    "<path fill='none' stroke='#22104A' stroke-width='8.4' stroke-linecap='round' stroke-linejoin='round' d='M37 27 H50 L37 41 H50'/>"
    "<path fill='none' stroke='#7A4ACC' stroke-width='5.2' stroke-linecap='round' stroke-linejoin='round' d='M37 27 H50 L37 41 H50'/>")

/* 3: poison, geo poison, taint - a glass flask holding a purple skull and crossbones */
ART(3,
    "<radialGradient id='gl' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#F6FAFF'/>"
    "<stop offset='0.5' stop-color='#AEBCD8'/>"
    "<stop offset='1' stop-color='#4E5A86'/>"
    "</radialGradient>"
    "<rect x='25' y='3' width='14' height='9' rx='2' fill='#C8A070' stroke='#1E1206' stroke-width='2'/>"
    "<path fill='url(#gl)' stroke='#0C1024' stroke-width='2.5' stroke-linejoin='round' d='M26 11 H38 V19 Q57 24 57 40 Q57 61 32 61 Q7 61 7 40 Q7 24 26 19 Z'/>"
    "<ellipse cx='20' cy='30' rx='5' ry='8' transform='rotate(25 20 30)' fill='#FFFFFF' fill-opacity='0.45'/>"
    "<path fill='none' stroke='#300820' stroke-width='7.8' stroke-linecap='round' stroke-linejoin='round' d='M19 37 L45 55 M45 37 L19 55'/>"
    "<path fill='none' stroke='#E83C9C' stroke-width='4.6' stroke-linecap='round' stroke-linejoin='round' d='M19 37 L45 55 M45 37 L19 55'/>"
    "<path fill='#8A2C9C' stroke='#22062A' stroke-width='2' stroke-linejoin='round' d='M32 25 C24 25 21 30 21 35 C21 39 23 41 26 42 V46 H38 V42 C41 41 43 39 43 35 C43 30 40 25 32 25 Z'/>"
    "<circle cx='27.5' cy='35' r='3' fill='#22062A'/>"
    "<circle cx='36.5' cy='35' r='3' fill='#22062A'/>")

/* 4: paralysis, geo paralysis - an olive-green skull with crackling sparks */
ART(4,
    "<radialGradient id='s' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#E4F0A0'/>"
    "<stop offset='0.5' stop-color='#8AA036'/>"
    "<stop offset='1' stop-color='#34440E'/>"
    "</radialGradient>"
    "<path fill='url(#s)' stroke='#121A04' stroke-width='2.5' stroke-linejoin='round' d='M32 4 C17 4 7.5 14 7.5 27 C7.5 35 10.5 40 15 43 L16 52 C16 56.5 19 59.5 23.5 59.5 L40.5 59.5 C45 59.5 48 56.5 48 52 L49 43 C53.5 40 56.5 35 56.5 27 C56.5 14 47 4 32 4 Z'/>"
    "<ellipse cx='25' cy='14' rx='10' ry='5' fill='#FFFFFF' fill-opacity='0.25'/>"
    "<ellipse cx='21.5' cy='30' rx='7.5' ry='7' fill='#1E2808'/>"
    "<ellipse cx='42.5' cy='30' rx='7.5' ry='7' fill='#1E2808'/>"
    "<path fill='#1E2808' stroke='#1E2808' stroke-width='1.5' stroke-linejoin='round' d='M32 36.5 L28.5 43.5 L35.5 43.5 Z'/>"
    "<rect x='21' y='47' width='22' height='8.5' rx='2' fill='#1E2808'/>"
    "<path fill='none' stroke='#8AA036' stroke-width='2' d='M26.5 47.5 V55 M32 47.5 V55 M37.5 47.5 V55'/>"
    "<path fill='none' stroke='#1A2604' stroke-width='6' stroke-linecap='round' stroke-linejoin='round' d='M3 44 L8 50 L4 54 L9 60'/>"
    "<path fill='none' stroke='#D6F040' stroke-width='3.2' stroke-linecap='round' stroke-linejoin='round' d='M3 44 L8 50 L4 54 L9 60'/>"
    "<path fill='none' stroke='#1A2604' stroke-width='6' stroke-linecap='round' stroke-linejoin='round' d='M61 44 L56 50 L60 54 L55 60'/>"
    "<path fill='none' stroke='#D6F040' stroke-width='3.2' stroke-linecap='round' stroke-linejoin='round' d='M61 44 L56 50 L60 54 L55 60'/>")

/* 5: blindness, flash - a dark grey swirling orb */
ART(5,
    "<radialGradient id='d' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#9A9AA2'/>"
    "<stop offset='0.5' stop-color='#4A4A52'/>"
    "<stop offset='1' stop-color='#121216'/>"
    "</radialGradient>"
    "<circle cx='32' cy='32' r='28' fill='url(#d)' stroke='#050506' stroke-width='2.6'/>"
    "<path fill='none' stroke='#D8D8E0' stroke-opacity='0.85' stroke-width='5' stroke-linecap='round' d='M32 32 Q40 26 34 20 Q24 14 17 24 Q11 36 22 45 Q34 53 45 43'/>"
    "<path fill='none' stroke='#08080A' stroke-width='4.5' stroke-linecap='round' d='M32 32 Q24 38 30 44 Q40 50 47 40 Q53 28 42 19 Q32 12 22 18'/>"
    "<circle cx='32' cy='32' r='4' fill='#08080A'/>"
    "<ellipse cx='26' cy='15.5' rx='12' ry='5' fill='#FFFFFF' fill-opacity='0.2'/>")

/* 6: silence, mute - a parchment scroll crossed out with a red X */
ART(6,
    "<radialGradient id='p' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#F2E2C0'/>"
    "<stop offset='0.5' stop-color='#C4A87A'/>"
    "<stop offset='1' stop-color='#7A6038'/>"
    "</radialGradient>"
    "<path fill='url(#p)' stroke='#1E1406' stroke-width='2.5' stroke-linejoin='round' d='M4 9 Q18 5 32 8 Q46 11 60 7 V55 Q46 59 32 56 Q18 53 4 57 Z'/>"
    "<path fill='none' stroke='#3A0408' stroke-width='12' stroke-linecap='round' stroke-linejoin='round' d='M15 16 L49 48 M49 16 L15 48'/>"
    "<path fill='none' stroke='#F0203C' stroke-width='8.5' stroke-linecap='round' stroke-linejoin='round' d='M15 16 L49 48 M49 16 L15 48'/>")

/* 7: petrification, gradual petrification - a grey stone skull */
ART(7,
    "<radialGradient id='s' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#F0F0F0'/>"
    "<stop offset='0.5' stop-color='#9C9C9C'/>"
    "<stop offset='1' stop-color='#444446'/>"
    "</radialGradient>"
    "<path fill='url(#s)' stroke='#0E0E10' stroke-width='2.5' stroke-linejoin='round' d='M32 4 C17 4 7.5 14 7.5 27 C7.5 35 10.5 40 15 43 L16 52 C16 56.5 19 59.5 23.5 59.5 L40.5 59.5 C45 59.5 48 56.5 48 52 L49 43 C53.5 40 56.5 35 56.5 27 C56.5 14 47 4 32 4 Z'/>"
    "<ellipse cx='25' cy='14' rx='10' ry='5' fill='#FFFFFF' fill-opacity='0.25'/>"
    "<path fill='none' stroke='#2A2A2C' stroke-width='1.6' stroke-linecap='round' stroke-linejoin='round' d='M40 6 L37 13 L41 17 M12 36 L17 38 L16 42'/>"
    "<ellipse cx='21.5' cy='30' rx='7.5' ry='7' fill='#1C1C1E'/>"
    "<ellipse cx='42.5' cy='30' rx='7.5' ry='7' fill='#1C1C1E'/>"
    "<path fill='#1C1C1E' stroke='#1C1C1E' stroke-width='1.5' stroke-linejoin='round' d='M32 36.5 L28.5 43.5 L35.5 43.5 Z'/>"
    "<rect x='21' y='47' width='22' height='8.5' rx='2' fill='#1C1C1E'/>"
    "<path fill='none' stroke='#9C9C9C' stroke-width='2' d='M26.5 47.5 V55 M32 47.5 V55 M37.5 47.5 V55'/>")

/* 8: disease, plague - a dark teal skull with red blotches */
ART(8,
    "<radialGradient id='s' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#7EB2C0'/>"
    "<stop offset='0.5' stop-color='#2E6676'/>"
    "<stop offset='1' stop-color='#0E2A34'/>"
    "</radialGradient>"
    "<path fill='url(#s)' stroke='#03101A' stroke-width='2.5' stroke-linejoin='round' d='M32 4 C17 4 7.5 14 7.5 27 C7.5 35 10.5 40 15 43 L16 52 C16 56.5 19 59.5 23.5 59.5 L40.5 59.5 C45 59.5 48 56.5 48 52 L49 43 C53.5 40 56.5 35 56.5 27 C56.5 14 47 4 32 4 Z'/>"
    "<ellipse cx='25' cy='14' rx='10' ry='5' fill='#FFFFFF' fill-opacity='0.25'/>"
    "<circle cx='20' cy='14' r='2.4' fill='#F06A78' stroke='#3A0A12' stroke-width='1'/>"
    "<circle cx='40' cy='11' r='2.2' fill='#F06A78' stroke='#3A0A12' stroke-width='1'/>"
    "<circle cx='48' cy='20' r='2' fill='#F06A78' stroke='#3A0A12' stroke-width='1'/>"
    "<circle cx='13' cy='24' r='2' fill='#F06A78' stroke='#3A0A12' stroke-width='1'/>"
    "<circle cx='32' cy='24' r='2.2' fill='#F06A78' stroke='#3A0A12' stroke-width='1'/>"
    "<circle cx='52' cy='36' r='2' fill='#F06A78' stroke='#3A0A12' stroke-width='1'/>"
    "<circle cx='14' cy='40' r='2' fill='#F06A78' stroke='#3A0A12' stroke-width='1'/>"
    "<circle cx='44' cy='47' r='1.8' fill='#F06A78' stroke='#3A0A12' stroke-width='1'/>"
    "<circle cx='19' cy='51' r='1.8' fill='#F06A78' stroke='#3A0A12' stroke-width='1'/>"
    "<ellipse cx='21.5' cy='30' rx='7.5' ry='7' fill='#061820'/>"
    "<ellipse cx='42.5' cy='30' rx='7.5' ry='7' fill='#061820'/>"
    "<path fill='#061820' stroke='#061820' stroke-width='1.5' stroke-linejoin='round' d='M32 36.5 L28.5 43.5 L35.5 43.5 Z'/>"
    "<rect x='21' y='47' width='22' height='8.5' rx='2' fill='#061820'/>"
    "<path fill='none' stroke='#2E6676' stroke-width='2' d='M26.5 47.5 V55 M32 47.5 V55 M37.5 47.5 V55'/>")

/* 9: curse, doom, curse II, bane, haunt - a violet screaming ghost */
ART(9,
    "<radialGradient id='v' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#F0E4FF'/>"
    "<stop offset='0.5' stop-color='#A07CDA'/>"
    "<stop offset='1' stop-color='#4A2A86'/>"
    "</radialGradient>"
    "<path fill='url(#v)' stroke='#14082A' stroke-width='2.5' stroke-linejoin='round' d='M32 3 C17 3 11 14 11 27 V44 Q4 46 4 52 Q4 60 13 59 Q19 58 22 54 Q27 61 33 57 Q38 61 43 55 Q48 60 54 58 Q61 55 58 47 Q55 43 53 42 V27 C53 14 47 3 32 3 Z'/>"
    "<ellipse cx='27' cy='12' rx='10' ry='4.5' fill='#FFFFFF' fill-opacity='0.3'/>"
    "<ellipse transform='translate(23.5 25) rotate(15)' rx='5' ry='7.5' fill='#1A0830'/>"
    "<ellipse transform='translate(40.5 25) rotate(-15)' rx='5' ry='7.5' fill='#1A0830'/>"
    "<ellipse cx='32' cy='42' rx='5.5' ry='8.5' fill='#1A0830'/>")

/* 10: stun, bind, terror - a red orb with a white figure standing rigid */
ART(10,
    "<radialGradient id='r' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#FFB4A4'/>"
    "<stop offset='0.5' stop-color='#D8302A'/>"
    "<stop offset='1' stop-color='#6A0806'/>"
    "</radialGradient>"
    "<circle cx='32' cy='32' r='29' fill='url(#r)' stroke='#1C0408' stroke-width='2.6'/>"
    "<ellipse cx='26' cy='15.5' rx='13' ry='6' fill='#FFFFFF' fill-opacity='0.3'/>"
    "<circle cx='32' cy='15' r='5.6' fill='#FFFFFF' stroke='#2A0404' stroke-width='1.8'/>"
    "<path fill='none' stroke='#2A0404' stroke-width='9.2' stroke-linecap='round' stroke-linejoin='round' d='M32 24 V39 M24 26 H40 M24 26 L22 40 M40 26 L42 40 M28 39 L27 54 M36 39 L37 54'/>"
    "<path fill='none' stroke='#FFFFFF' stroke-width='5.6' stroke-linecap='round' stroke-linejoin='round' d='M32 24 V39 M24 26 H40 M24 26 L22 40 M40 26 L42 40 M28 39 L27 54 M36 39 L37 54'/>")

/* 12: weight, geo weight - an olive orb with a figure dragged down by two blue weights */
ART(12,
    "<radialGradient id='y' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#F4F4B4'/>"
    "<stop offset='0.5' stop-color='#B4B03A'/>"
    "<stop offset='1' stop-color='#4A4A0C'/>"
    "</radialGradient>"
    "<circle cx='32' cy='32' r='29' fill='url(#y)' stroke='#141402' stroke-width='2.6'/>"
    "<ellipse cx='26' cy='15.5' rx='13' ry='6' fill='#FFFFFF' fill-opacity='0.3'/>"
    "<circle cx='29' cy='14' r='5' fill='#FFFFFF' stroke='#141402' stroke-width='1.8'/>"
    "<path fill='none' stroke='#141402' stroke-width='8.6' stroke-linecap='round' stroke-linejoin='round' d='M31 21 L36 36 M33 26 L16 34 M33 26 L47 33 M36 36 L28 54 M36 36 L42 54'/>"
    "<path fill='none' stroke='#FFFFFF' stroke-width='5' stroke-linecap='round' stroke-linejoin='round' d='M31 21 L36 36 M33 26 L16 34 M33 26 L47 33 M36 36 L28 54 M36 36 L42 54'/>"
    "<path fill='#2848E8' stroke='#020A30' stroke-width='1.8' stroke-linejoin='round' d='M14 35 Q8 42 8 49 Q8 56 14 56 Q20 56 20 49 Q20 42 14 35 Z'/>"
    "<ellipse cx='12' cy='45' rx='1.6' ry='3.5' fill='#FFFFFF' fill-opacity='0.5'/>"
    "<path fill='#2848E8' stroke='#020A30' stroke-width='1.8' stroke-linejoin='round' d='M50 35 Q44 42 44 49 Q44 56 50 56 Q56 56 56 49 Q56 42 50 35 Z'/>"
    "<ellipse cx='48' cy='45' rx='1.6' ry='3.5' fill='#FFFFFF' fill-opacity='0.5'/>")

/* 13: slow, geo slow - a pale orb with a blue arrow sinking down and right */
ART(13,
    "<radialGradient id='w' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#FFFFFF'/>"
    "<stop offset='0.5' stop-color='#C8D8EE'/>"
    "<stop offset='1' stop-color='#6A86B4'/>"
    "</radialGradient>"
    "<circle cx='32' cy='32' r='29' fill='url(#w)' stroke='#060E24' stroke-width='2.6'/>"
    "<ellipse cx='26' cy='15.5' rx='13' ry='6' fill='#FFFFFF' fill-opacity='0.3'/>"
    "<path fill='none' stroke='#3A2A00' stroke-width='5.2' stroke-linecap='round' stroke-linejoin='round' d='M38 12 L45 9 M44 18 L50 15 M14 40 L20 38 M18 48 L24 46'/>"
    "<path fill='none' stroke='#F0C440' stroke-width='2.8' stroke-linecap='round' stroke-linejoin='round' d='M38 12 L45 9 M44 18 L50 15 M14 40 L20 38 M18 48 L24 46'/>"
    "<path fill='#1840E0' stroke='#020A30' stroke-width='2' stroke-linejoin='round' d='M15.5 21 L21 15.5 L38.5 33 L43 28.5 L48 48 L28.5 43 L33 38.5 Z'/>")

/* 14: charm, charm II - a dark crimson orb with a big pink heart */
ART(14,
    "<radialGradient id='m' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#C04A5E'/>"
    "<stop offset='0.5' stop-color='#6A1426'/>"
    "<stop offset='1' stop-color='#26040C'/>"
    "</radialGradient>"
    "<circle cx='32' cy='32' r='29' fill='url(#m)' stroke='#100204' stroke-width='2.6'/>"
    "<ellipse cx='26' cy='15.5' rx='13' ry='6' fill='#FFFFFF' fill-opacity='0.3'/>"
    "<radialGradient id='h' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#FFE0F0'/>"
    "<stop offset='0.5' stop-color='#F47AAC'/>"
    "<stop offset='1' stop-color='#B02A64'/>"
    "</radialGradient>"
    "<path transform='translate(28 12) scale(1.45)' fill='url(#h)' stroke='#3A0418' stroke-width='1.5' stroke-linejoin='round' d='M0 6 C-3 -1 -12 -1 -12 6 C-12 12 -5 16 0 22 C5 16 12 12 12 6 C12 -1 3 -1 0 6 Z'/>"
    "<path transform='translate(46 40) scale(0.6)' fill='#C83A6A' stroke='#3A0418' stroke-width='3' stroke-linejoin='round' d='M0 6 C-3 -1 -12 -1 -12 6 C-12 12 -5 16 0 22 C5 16 12 12 12 6 C12 -1 3 -1 0 6 Z'/>")

/* 16: amnesia - an open hand struck through with a red bar */
ART(16,
    "<radialGradient id='k' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#FFE8B8'/>"
    "<stop offset='0.5' stop-color='#D8A060'/>"
    "<stop offset='1' stop-color='#7A4A1A'/>"
    "</radialGradient>"
    "<path fill='url(#k)' stroke='#2A1606' stroke-width='2.4' stroke-linejoin='round' d='M14 32 V12 Q14 8 18 8 Q22 8 22 12 V28 V8 Q22 4 26 4 Q30 4 30 8 V28 V7 Q30 3 34 3 Q38 3 38 7 V28 V11 Q38 7 42 7 Q46 7 46 11 V36 L51 30 Q55 26 58 29 Q60 31 57 35 L47 52 Q43 59 34 59 H24 Q14 59 14 48 Z'/>"
    "<path fill='none' stroke='#5A3410' stroke-width='1.6' stroke-linecap='round' d='M22 14 V28 M30 13 V28 M38 15 V28'/>"
    "<path fill='none' stroke='#300404' stroke-width='10' stroke-linecap='round' stroke-linejoin='round' d='M6 58 L60 6'/>"
    "<path fill='none' stroke='#E02A2A' stroke-width='6.5' stroke-linecap='round' stroke-linejoin='round' d='M6 58 L60 6'/>")

/* 27: food - a hamburger */
ART(27,
    "<radialGradient id='bun' cx='40%' cy='25%' r='80%' fx='40%' fy='25%'>"
    "<stop offset='0' stop-color='#FFE6A0'/>"
    "<stop offset='0.5' stop-color='#E09A38'/>"
    "<stop offset='1' stop-color='#8A4A0C'/>"
    "</radialGradient>"
    "<path fill='#C88A3A' stroke='#2A1404' stroke-width='2.4' stroke-linejoin='round' d='M7 46 H57 Q57 57 47 57 H17 Q7 57 7 46 Z'/>"
    "<path fill='#6A2E14' stroke='#1E0804' stroke-width='2.4' stroke-linejoin='round' d='M6 39 Q6 35 10 35 H54 Q58 35 58 39 V44 Q58 48 54 48 H10 Q6 48 6 44 Z'/>"
    "<path fill='#5CB83A' stroke='#0C2204' stroke-width='2' stroke-linejoin='round' d='M5 33 L11 38 L17 33 L23 38 L29 33 L35 38 L41 33 L47 38 L53 33 L59 37 V31 H5 Z'/>"
    "<path fill='url(#bun)' stroke='#2A1404' stroke-width='2.4' stroke-linejoin='round' d='M5 32 Q5 7 32 7 Q59 7 59 32 Z'/>"
    "<ellipse cx='24' cy='15' rx='11' ry='4' fill='#FFFFFF' fill-opacity='0.35'/>"
    "<ellipse cx='20' cy='22' rx='1.8' ry='1' fill='#FFF6D8'/>"
    "<ellipse cx='30' cy='17' rx='1.8' ry='1' fill='#FFF6D8'/>"
    "<ellipse cx='40' cy='21' rx='1.8' ry='1' fill='#FFF6D8'/>"
    "<ellipse cx='46' cy='27' rx='1.8' ry='1' fill='#FFF6D8'/>"
    "<ellipse cx='26' cy='27' rx='1.8' ry='1' fill='#FFF6D8'/>"
    "<ellipse cx='36' cy='26' rx='1.8' ry='1' fill='#FFF6D8'/>")

/* 32: flee, sprint, quickening - a blue orb with a running figure and a red arrow */
ART(32,
    "<radialGradient id='f' cx='38%' cy='30%' r='75%' fx='38%' fy='30%'>"
    "<stop offset='0' stop-color='#ECECFF'/>"
    "<stop offset='0.5' stop-color='#8A8CEA'/>"
    "<stop offset='1' stop-color='#2E2E8A'/>"
    "</radialGradient>"
    "<circle cx='32' cy='32' r='29' fill='url(#f)' stroke='#06061E' stroke-width='2.6'/>"
    "<ellipse cx='26' cy='15.5' rx='13' ry='6' fill='#FFFFFF' fill-opacity='0.3'/>"
    "<circle cx='25' cy='14' r='5' fill='#FFFFFF' stroke='#06061E' stroke-width='1.8'/>"
    "<path fill='none' stroke='#06061E' stroke-width='8.6' stroke-linecap='round' stroke-linejoin='round' d='M28 21 L36 34 M30 24 L19 30 M30 24 L42 26 M36 34 L26 41 L22 48 M36 34 L46 44'/>"
    "<path fill='none' stroke='#FFFFFF' stroke-width='5' stroke-linecap='round' stroke-linejoin='round' d='M28 21 L36 34 M30 24 L19 30 M30 24 L42 26 M36 34 L26 41 L22 48 M36 34 L46 44'/>"
    "<path fill='#F01818' stroke='#300404' stroke-width='1.8' stroke-linejoin='round' d='M10 51 L19 44.5 V48.5 H50 V53.5 H19 V57.5 Z'/>")
