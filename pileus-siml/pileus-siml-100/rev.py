import random

filp = open('rev.txt','a')
for i in range(61, 3001):
    rand = random.randint(1,3) 
    num = 257 + rand
    filp.write('%s\t%s\n'%(i, num))
filp.close()
