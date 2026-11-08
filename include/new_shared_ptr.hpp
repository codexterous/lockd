/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Codexterous, Inc - Software. Thought through.
 * http://www.codexterous.com
 *
 * File       : new_shared_ptr.hpp
 * Author     : bobbyrward (http://www.boostcookbook.com/User:/bobbyrward) with contributions
 *              by Kirit (http://www.boostcookbook.com/User:/Kirit)
 * Part Of    : lockd, an interprocess locking daemon
 * Purpose    : Factory for standard shared pointers with private construction and destruction.
 * Start Date : 05/12/2009
 *
 * Description: Taken from the Boost Cookbook website (www.boostcookbook.com) at
 *              http://www.boostcookbook.com/Recipe:/1234950.  It frees the end user of shared_ptr
 *              from the need to create an instance manually on the heap or stack, and from
 *              deleting a shared_ptr manually using "delete get()".
 *
 *              It also gives the user the ability to make "new_ptr_access" a friend and then
 *              declare the ctor and dtor of the class private.
 *
 * Example    : class foo {
 *                  private:
 *                      foo():name_("My Name"){}
 *                      foo(const std::string name): name_(name) {}
 *                      foo(int i, int j): name_("I & J") {}
 *                      virtual ~foo() = default;
 *                      friend class new_ptr_access;
 *                  public:
 *                      const std::string& name() const { return name_; }
 *                  private:
 *                      std::string name_;
 *              };
 *
 *              shared_ptr<foo> bar1 = new_ptr<foo>();
 *              shared_ptr<foo> bar2 = new_ptr<foo>("example");
 *              shared_ptr<foo> bar3 = new_ptr<foo>(1, 2);
 *
 *              delete bar1.get(); // won't compile
 *
 *              foo stackInstance("example"); // won't compile
 *              foo* heapInstance = new foo("example") // won't compile
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if !defined(LOCKD_LOCKD_NEW_SHARED_PTR_HPP_INCLUDED_)
# define LOCKD_LOCKD_NEW_SHARED_PTR_HPP_INCLUDED_

# include <memory>
# include <utility>

namespace lkd {

class new_ptr_access
{
    /* construction and destruction */
public:
    new_ptr_access() = delete;
    virtual ~new_ptr_access() = default;

    /* private methods */
private:
    template <typename T> static void destroy(T* const ptr)
    {
        static_assert(sizeof(T), "The managed type must be complete when deleted.");
        delete ptr;
    }
    template <typename T, typename... Args> static std::shared_ptr<T> create(Args&&... args)
    {
        // Both allocation and deletion occur with this friend's access. make_shared cannot
        // invoke a private constructor or destructor, even when called from a friend.
        return std::shared_ptr<T>(new T(std::forward<Args>(args)...), destroy<T>);
    }
    template <typename T, typename... Args> friend std::shared_ptr<T> new_ptr(Args&&... args);
};


template <typename T, typename... Args> std::shared_ptr<T> new_ptr(Args&&... args)
{
    return new_ptr_access::create<T>(std::forward<Args>(args)...);
}

} // namespace lkd

#endif // !defined(LOCKD_LOCKD_NEW_SHARED_PTR_HPP_INCLUDED_)
