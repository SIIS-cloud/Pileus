import operator
import string
print ("input n and k:")
strn=raw_input("n:")
strk=raw_input("k:")
n=string.atoi(strn)
k=string.atoi(strk)
def c(n,k):  
    return  reduce(operator.mul, range(n - k + 1, n + 1)) / reduce(operator.mul, range(1, k +1))  
def fac(n):  
    return  reduce(operator.mul, range(1,n+1))  
def a(n,k):  
    return  reduce(operator.mul, range(n - k + 1, n + 1))

if __name__ == '__main__':  
    cc=c(n,k)  
    aa=a(n,k)  
    fa=fac(n)    
    print ("c(n,k)= %s" %cc)  
    print ("a(n,k)= %s" %aa)   
    print ("fac(n,k)= %s" %fa)   
