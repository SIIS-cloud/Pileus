import string

filp = open("temp.txt",'r')
ret = 0
for line in filp:
    l = line.split()
    num = l[0]
    node = l[1]
    a = string.atof(node) * 1/80
    #print 'a is %s'%a
    ret = ret + a

print 'ret is %s'%ret
