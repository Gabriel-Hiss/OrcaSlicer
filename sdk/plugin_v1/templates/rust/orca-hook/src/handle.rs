use std::marker::PhantomData;
use std::ops::{Deref, DerefMut};

/// Borrowed handle for C++ aggregates that are not trivially-copyable.
/// Never owns, never drops the pointee. Borrowed for the hook callback
/// lifetime only; storing beyond is unsound.
pub struct Borrowed<'a, T> {
    ptr: *const T,
    _lt: PhantomData<&'a T>,
}
impl<'a, T> Borrowed<'a, T> {
    /// # Safety — ptr must be valid, borrowed, and not freed for 'a.
    pub unsafe fn from_raw(ptr: *const T) -> Self {
        Self { ptr, _lt: PhantomData }
    }
    pub fn get(&self) -> *const T { self.ptr }
    pub fn is_null(&self) -> bool { self.ptr.is_null() }
    /// # Safety — must guarantee dereference validity.
    pub unsafe fn as_ref(&self) -> Option<&'a T> {
        self.ptr.as_ref()
    }
}
impl<'a, T> Deref for Borrowed<'a, T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.ptr }
    }
}

/// Mutable borrowed handle.
pub struct BorrowedMut<'a, T> {
    ptr: *mut T,
    _lt: PhantomData<&'a mut T>,
}
impl<'a, T> BorrowedMut<'a, T> {
    /// # Safety — ptr must be valid, borrowed, exclusive for 'a.
    pub unsafe fn from_raw(ptr: *mut T) -> Self {
        Self { ptr, _lt: PhantomData }
    }
    pub fn get(&self) -> *mut T { self.ptr }
    /// # Safety
    pub unsafe fn as_mut(&mut self) -> Option<&'a mut T> {
        self.ptr.as_mut()
    }
}
impl<'a, T> Deref for BorrowedMut<'a, T> {
    type Target = T;
    fn deref(&self) -> &T { unsafe { &*self.ptr } }
}
impl<'a, T> DerefMut for BorrowedMut<'a, T> {
    fn deref_mut(&mut self) -> &mut T { unsafe { &mut *self.ptr } }
}
