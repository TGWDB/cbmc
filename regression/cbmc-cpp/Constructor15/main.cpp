class A
{
public:
  A(){};

private:
  A(const A &);            // disabled
  A &operator=(const A &); // disabled
};

class B : public A
{
};

int main()
{
  B b; // ok
}
