#include<stdio.h>
int sum(int n){ 
    int s=0; 
    for(int i=0;i<n;i++) 
        s+=i; 
    return s; 
}
int main(){
    int n=10;
    for(int i=0;i<n;i++){
        printf("%d \n",i);
    }
    sum(10);
    return 0;
}

